#include <stdio.h>
#include <string.h>
#include "wifi-probe.h"

int main(void)
{
    struct wp_db *db = wp_db_open(":memory:");
    if (!db) { printf("open fail\n"); return 1; }
    if (wp_db_store_device(db, "abc", 100, 200, -55, -70, WP_RSSI_MID, "Home", 0) != 0) {
        printf("dev fail\n"); return 2;
    }
    if (wp_db_store_visit(db, "abc", 100, 200, "Home", WP_RSSI_MID, 0) != 0) {
        printf("visit fail\n"); return 3;
    }
    int rc = wp_db_query_json(db, "device", 0, 0);
    wp_db_close(db);
    printf("db query rc=%d\n", rc);
    return (rc == 0) ? 0 : 4;
}
