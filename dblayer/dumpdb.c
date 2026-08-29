#include <stdio.h>
#include <stdlib.h>
#include "codec.h"
#include "tbl.h"
#include "util.h"
#include "../pflayer/pf.h"
#include "../amlayer/am.h"
#define checkerr(err) {if (err < 0) {PF_PrintError(); exit(1);}}


void
printRow(void *callbackObj, RecId rid, byte *row, int len) {
    Schema *schema = (Schema *) callbackObj;
    byte *cursor = row;

    for (int i = 0; i < schema->numColumns; i++){
        ColumnDesc *col = schema->columns[i];

        if(col->type == INT){

            int val = DecodeInt(cursor);
            printf("%d", val);
            cursor += 4;
        }
        else if(col->type == LONG){
            
            long long val = DecodeLong(cursor);
            printf("%lld", val);
            cursor += 8;
        }
        else{

            short strLen = DecodeShort(cursor);
            char buf[PF_PAGE_SIZE];
            DecodeCString(cursor, buf, sizeof(buf));
            printf("%s", buf);
            cursor += strLen + 2;
        }
        if( i < schema->numColumns - 1){
            printf(",");
        }
        
    } 
    printf("\n");
}

#define DB_NAME "data.db"
#define INDEX_NAME "data.db.0"
	 
void
index_scan(Table *tbl, Schema *schema, int indexFD, int op, int value) {
   int scanDesc = AM_OpenIndexScan(indexFD, 'i', sizeof(int), op, (char *)&value);
   if(scanDesc < 0){
        AM_PrintError("AM_OpenIndexScan failed");
        exit(1);
   }
   int rid;
   while((rid = AM_FindNextEntry(scanDesc )) != AME_EOF){

    if(rid < 0){
        AM_PrintError("AM_FindNextEntry failed");
        exit(1);
    }
    byte buffer[PF_PAGE_SIZE];
    int len = Table_Get(tbl, rid, buffer, sizeof(buffer));
    checkerr(len);
    printRow(schema, rid, buffer, len);
   }
   AM_CloseIndexScan(scanDesc);
}

int
main(int argc, char **argv) {
    char *schemaTxt = "Country:varchar,Capital:varchar,Population:int";
    Schema *schema = parseSchema(schemaTxt);
    Table *tbl;

    int err = Table_Open(DB_NAME, schema, false, &tbl);
    checkerr(err);
    if (argc == 2 && *(argv[1]) == 's') {
        Table_Scan(tbl, schema, printRow);

    } else {
	// index scan by default
	int indexFD = PF_OpenFile(INDEX_NAME);
	checkerr(indexFD);

	// Ask for populations less than 100000, then more than 100000. Together they should
	// yield the complete database.
	index_scan(tbl, schema, indexFD, LESS_THAN_EQUAL, 100000);
	index_scan(tbl, schema, indexFD, GREATER_THAN, 100000);
    }
    Table_Close(tbl);
    return 0;
}
