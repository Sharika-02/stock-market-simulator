#include <sqlite3.h>
#include <stdio.h>
#include "database.h"

sqlite3 *db;

void init_db() {

    if(sqlite3_open("stocks.db", &db)) {
        printf("Database open error\n");
        return;
    }

    printf("Database connected\n");
}

void insert_price(char *symbol, float price) {

    char sql[256];

    sprintf(sql,
        "INSERT INTO price_history(symbol,price) VALUES('%s',%f);",
        symbol, price);

    sqlite3_exec(db, sql, 0, 0, 0);
}

void log_event(char *ip, char *event) {

    char sql[256];

    sprintf(sql,
        "INSERT INTO session_logs(client_ip,event) VALUES('%s','%s');",
        ip, event);

    sqlite3_exec(db, sql, 0, 0, 0);
}