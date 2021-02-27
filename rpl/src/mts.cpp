#include "../include/mts.h"
#include "event_handle.h"

namespace rpl{
    int MTS_Handler::init() {
        mysql = mysql_init(nullptr);
        mysql->reconnect = 1;
        mysql = mysql_real_connect(mysql, "10.24.10.113", "ssh", "ssh",
                                   NULL, 3306, NULL, 0);
        if (!mysql){
            printf("Error connecting to database:%s\n",mysql_error(mysql));
            return -1;
        }
        mysql_query(mysql,"SHOW MASTER STATUS;");
        MYSQL_RES *res = mysql_store_result(mysql);
        MYSQL_ROW row = mysql_fetch_row(res);
        rpl.start_position = 4U;
        rpl.server_id =1;
        rpl.file_name = row[0];
        rpl.file_name_length = strlen(rpl.file_name);
        rpl.start_position = atol(row[1]);
        rpl.flags = MYSQL_RPL_SKIP_HEARTBEAT;
        return 0;
    }

    int MTS_Handler::run() {
        {
            if (mysql_binlog_open(mysql, &rpl))
            {
                fprintf(stderr, "mysql_binlog_open() failed\n");
                fprintf(stderr, "Error %u: %s\n",
                        mysql_errno(mysql), mysql_error(mysql));
                exit(1);
            }
            for (;;)  /* read events until error or EOF */
            {
                if (mysql_binlog_fetch(mysql, &rpl)) {
                    fprintf(stderr, "mysql_binlog_fetch() failed\n");
                    fprintf(stderr, "Error %u: %s\n",
                            mysql_errno(mysql), mysql_error(mysql));
                    break;
                }
                if (rpl.size == 0)  /* EOF */
                {
                    fprintf(stderr, "EOF event received\n");
                    break;
                }
                process();
            }
        }while (false);
        mysql_binlog_close(mysql, &rpl);
        mysql_close(mysql);
    }

    int MTS_Handler::process() {
        binary_log::Log_event_type type = (binary_log::Log_event_type)rpl.buffer[1 + EVENT_TYPE_OFFSET];
        switch (type) {
            case binary_log::FORMAT_DESCRIPTION_EVENT: {
                printf("FORMAT_DESCRIPTION_EVENT!\n");
                binary_log::Format_description_event *fde_tmp = new binary_log::Format_description_event(4, "8.017");
                char *buf = (char *) malloc(sizeof(char *) * (rpl.size + 1));
                memcpy(buf, rpl.buffer + 1, rpl.size - 1);
                fde = new binary_log::Format_description_event(reinterpret_cast<const char *>(rpl.buffer + 1), fde_tmp);
                fde->print_event_info(std::cout);
                break;
            }
            case binary_log::TABLE_MAP_EVENT:{
                printf("TABLE_MAP_EVENT!\n");
                auto *ev = new binary_log::Table_map_event(reinterpret_cast<const char *>(rpl.buffer + 1), fde);
                std::string full_table_name = ev->get_db_name()+'.'+ev->get_table_name();
                auto it = tables.find(full_table_name);
                //如果没有这个表则创建这个表，并构建schema
                if (it == tables.end()){
                    auto *table_schema = eventHandler.unpack(ev);
                    Table *table = new Table(full_table_name);
                    table->schema = new binary_log::TableSchema(*table_schema);
                    table->_pk = table_schema->getPrikey();
                    tables[full_table_name] = table;
                }
                break;
            }
//            下面三个rows event完全可以使用一套代码，主要处理逻辑的差别在Event_Handler中的unpack中
//             但是考虑到后面可能会加不一样的处理逻辑，在这是分开处理的
            case binary_log::WRITE_ROWS_EVENT: {
                printf("WRITE_ROWS_EVENTS!\n");
                auto *ev = new binary_log::Write_rows_event(reinterpret_cast<const char *>(rpl.buffer + 1), fde);
                char *buf = new char[ev->row.size()];
                std::copy(ev->row.begin(), ev->row.end(), buf);
                auto reader = binary_log::Event_reader(buf, ev->row.size());
                auto *table_schema = eventHandler.get_schema(ev->get_table_id());
                std::string table_full_name = table_schema->getDBname() + '.' + table_schema->getTablename();
                auto it = tables.find(table_full_name);
                if (it==tables.end()){
                    printf("No Match Table!\n");
                }
                Table *table = it->second;
                int idx = table_schema->getIdxByName(table_schema->getPrikey());
                auto col = eventHandler.unpack(ev, reader, table_schema);
                Row *row = new Row(col[idx], ev->header()->when.tv_sec, false, table_schema->getDBname(), table_schema->getTablename());
                row->columns = col;
                table->insert_row(row);
                break;
            }
            case binary_log::DELETE_ROWS_EVENT:{
                printf("DELETE_ROWS_EVENT!\n");
                auto *ev = new binary_log::Delete_rows_event(reinterpret_cast<const char *>(rpl.buffer + 1), fde);
                char *buf = new char[ev->row.size()];
                std::copy(ev->row.begin(), ev->row.end(), buf);
                auto reader = binary_log::Event_reader(buf, ev->row.size());
                auto *table_schema = eventHandler.get_schema(ev->get_table_id());
                std::string table_full_name = table_schema->getDBname() + '.' + table_schema->getTablename();
                auto it = tables.find(table_full_name);
                if (it==tables.end()){
                    printf("No Match Table!\n");
                }
                Table *table = it->second;
                int idx = table_schema->getIdxByName(table_schema->getPrikey());
                auto col = eventHandler.unpack(ev, reader, table_schema);
                Row *row = new Row(col[idx], ev->header()->when.tv_sec, true, table_schema->getDBname(), table_schema->getTablename());
                row->columns = col;
                table->insert_row(row);
                break;
            }
            case binary_log::UPDATE_ROWS_EVENT:{
                printf("UPDATE_ROWS_EVENT!\n");
                auto *ev = new binary_log::Update_rows_event(reinterpret_cast<const char *>(rpl.buffer + 1), fde);
                char *buf = new char[ev->row.size()];
                std::copy(ev->row.begin(), ev->row.end(), buf);
                auto reader = binary_log::Event_reader(buf, ev->row.size());
                auto *table_schema = eventHandler.get_schema(ev->get_table_id());
                std::string table_full_name = table_schema->getDBname() + '.' + table_schema->getTablename();
                auto it = tables.find(table_full_name);
                if (it==tables.end()){
                    printf("No Match Table!\n");
                }
                Table *table = it->second;
                int idx = table_schema->getIdxByName(table_schema->getPrikey());
                auto col = eventHandler.unpack(ev, reader, table_schema);
                Row *row = new Row(col[idx], ev->header()->when.tv_sec, true, table_schema->getDBname(), table_schema->getTablename());
                row->columns = col;
                table->insert_row(row);
                break;
//            auto it = eh.unpack(ev, reader, table);
            }
            default:
                printf("Other Events!\n");
                break;
        }
    }
}