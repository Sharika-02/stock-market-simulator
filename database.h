#ifndef DATABASE_H
#define DATABASE_H
extern sqlite3 *db;
void init_db();
void insert_price(char *symbol, float price);
void log_event(char *ip, char *event);

#endif