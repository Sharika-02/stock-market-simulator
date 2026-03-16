#ifndef DATABASE_H
#define DATABASE_H

void init_db();
void insert_price(char *symbol, float price);
void log_event(char *ip, char *event);

#endif