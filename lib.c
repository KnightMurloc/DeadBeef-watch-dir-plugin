//
// Created by victor on 22.01.2022.
//

//need for nftw
#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64
#define _XOPEN_SOURCE 700

#include <deadbeef/deadbeef.h>
#include "utils.h"
#include <unistd.h>
#include <glib.h>
#include <sqlite3.h>
#include <sys/inotify.h>
#include <ftw.h>
#include <libgen.h>
#include <poll.h>

//need for nftw
#ifndef USE_FDS
#define USE_FDS 15
#endif

#define MAX_EVENTS 1024 /*Максимальное кличество событий для обработки за один раз*/
#define LEN_NAME 256 /*Будем считать, что длина имени файла не превышает 16 символов*/
#define EVENT_SIZE  ( sizeof (struct inotify_event) ) /*размер структуры события*/
#define BUF_LEN     ( MAX_EVENTS * ( EVENT_SIZE + LEN_NAME )) /*буфер для хранения данных о событиях*/


static DB_functions_t *deadbeef;
static sqlite3* db;
static char *err_msg = 0;
static int in_fd; //inotify fd
static GHashTable* wd_table;
static GHashTable* wait_table;
static intptr_t watch_thread_ptr;
static int shutdown = 0;
static const int notify_mask = IN_CREATE | IN_CLOSE_WRITE | IN_DELETE | IN_MOVE;

static const char* root = ""; //TODO временое решение
static const char* ext_list = "mp3,flac,ape,m4a,wav"; //TODO временое решение

int file_callback(const char *filepath, const struct stat *info,
                  int typeflag, struct FTW *pathinfo){
    if(typeflag == FTW_D){
        int* wd = malloc(sizeof(int));

        *wd = inotify_add_watch(in_fd, filepath, notify_mask);
        g_hash_table_insert(wd_table,wd,strdup(filepath));
    }else{
        char* dir_name_tmp = strdup(filepath);
        char* basename_tmp = strdup(filepath);

        char* file = basename(basename_tmp);

        if(strstr(ext_list, get_filename_ext(file)) == NULL){
            free(dir_name_tmp);
            free(basename_tmp);
            return 0;
        }

        const char* sql_form = "insert into files_tmp(path) values('%s');";
        char* sql = malloc(strlen(sql_form) + strlen(filepath) + 2);
        sprintf(sql,sql_form,filepath);

        int rc = sqlite3_exec(db, sql, 0, 0, &err_msg);

        if (rc != SQLITE_OK ) {
            fprintf(stderr, "SQL error2: %s\n", err_msg);
        }

        free(sql);
        free(dir_name_tmp);
        free(basename_tmp);
    }

    return 0;
}

static ddb_playlist_t* find_playlist(const char* find_title){
    int pl_count = deadbeef->plt_get_count();
    printf("pl count: %d\n", pl_count);

    for(int i = 0; i < pl_count; i++){
        ddb_playlist_t* pl = deadbeef->plt_get_for_idx(i);

        char title[1024];

        deadbeef->plt_get_title(pl,title,sizeof(title));
        if(strcmp(title,find_title) == 0){
            return pl;
        }
    }
    return NULL;
}

void free_wd(gpointer data){
    printf("wd free\n");
    inotify_rm_watch(in_fd,  *((int*) data));
    free(data);
}

static int start(){
    //get path to database file
    const char* db_dir = deadbeef->get_system_dir(DDB_SYS_DIR_CONFIG);
    char* db_file = full_path(db_dir,"data.db");
    printf("%s\n",db_file);

    int init_needed = access(db_file,R_OK);
    printf("init_needed: %d\n", init_needed);

    int rc = sqlite3_open(db_file, &db);

    if (rc != SQLITE_OK) {
        printf("Cannot open database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return 1;
    }

    if(init_needed == -1){
        printf("init bd\n");
        sqlite3_exec(db,"CREATE TABLE \"files\" (\n"
                        "\t\"path\"\tTEXT NOT NULL CHECK(path <> \"\")\n"
                        ");",NULL,NULL,&err_msg);
        sqlite3_exec(db,"CREATE INDEX \"path_index\" ON \"files\" (\n"
                        "\t\"path\"\n"
                        ");", NULL,NULL,&err_msg);
    }else{
        printf("work with exist db\n");
    }

    sqlite3_exec(db, "CREATE temporary TABLE \"files_tmp\" (\n"
                     "\t\"path\"\tTEXT NOT NULL CHECK(path <> \"\")\n"
                     ");",NULL,NULL, &err_msg);

    printf("init wd end\n");

    in_fd = inotify_init();
    if ( in_fd < 0 ) {
        perror( "Couldn't initialize inotify");
    }

    wd_table = g_hash_table_new_full(g_int_hash,g_int_equal,free_wd,g_free);
    wait_table = g_hash_table_new(g_str_hash,g_str_equal);

    nftw(root, file_callback, USE_FDS, FTW_PHYS);

    free(db_file);
    return 0;
}

static int add_callback(void *pl_raw, int argc, char **argv, char **azColName){
    ddb_playlist_t* pl = pl_raw;
    if(argc == 1){
        const char* sql_form = "insert into files(path) values('%s');";
        const char* path = argv[0];
        printf("%s\n",path);
        char* sql = malloc(strlen(sql_form) + strlen(path) + 2);
        sprintf(sql,sql_form,path);

        int rc = sqlite3_exec(db, sql, 0, 0, &err_msg);

        if (rc != SQLITE_OK ) {
            fprintf(stderr, "SQL error: %s\n", err_msg);
        }


        deadbeef->plt_add_file2(1,pl,path,NULL,NULL);
//        deadbeef->plt_insert_file2(1,pl,deadbeef->plt_get_last(pl,0),path,NULL,NULL,NULL);

        free(sql);
    }
    return 0;
}

static ddb_playItem_t* find_by_path(ddb_playlist_t* pl, const char* path){
    int count = deadbeef->plt_get_item_count(pl,0);
    printf("count: %d\n",count);
    ddb_playItem_t* current = deadbeef->plt_get_first(pl,0);

    deadbeef->pl_lock();
    while (current){
//        printf("test\n");

        const char* path_ = deadbeef->pl_find_meta(current,":URI");
//        printf("path_: %s : %s\n", path_, path);
        if(strcmp(path, path_) == 0){
            deadbeef->pl_item_unref(current);
            deadbeef->pl_unlock();
            return current;
        }

        ddb_playItem_t* tmp = current;
        current = deadbeef->pl_get_next(current,0);
        deadbeef->pl_item_unref(tmp);
    }
    deadbeef->pl_unlock();
    return NULL;
}

static int remove_callback(void *pl, int argc, char **argv, char **azColName){
    if(argc == 1){
        const char* sql_form = "delete from files where path = '%s';";
        const char* path = argv[0];
        printf("%s\n", path);
        char* sql = malloc(strlen(sql_form) + strlen(path) + 2);
        sprintf(sql,sql_form,path);

        int rc = sqlite3_exec(db, sql, 0, 0, &err_msg);

        if (rc != SQLITE_OK ) {
            fprintf(stderr, "SQL error: %s\n", err_msg);
        }
        printf("remove from db!\n");

        ddb_playItem_t* playItem = find_by_path(pl,path);
        printf("finded: %p\n", playItem);
        if(playItem == NULL){
            free(sql);
            return 0;
        }

        deadbeef->pl_lock();
        deadbeef->pl_item_ref(playItem);

        deadbeef->plt_remove_item(pl,playItem);
        printf("removed\n");
//
        deadbeef->pl_item_unref(playItem);
        deadbeef->pl_unlock();

        free(sql);
    }
    return 0;
}

void add_dir(struct inotify_event *event){

    const char* parent_name = g_hash_table_lookup(wd_table,&event->wd);

    char* path = malloc(strlen(parent_name) + 1 + strlen(event->name) + 2);

    sprintf(path,"%s/%s",parent_name,event->name);
//    printf("test %s\n", path);
    int* wd = malloc(sizeof(int));

    *wd = inotify_add_watch(in_fd, path, notify_mask);
    if (*wd == -1)
    {
        printf("Couldn't add watch to %s\n", path);
    }
    g_hash_table_insert(wd_table,wd,path);
}

void add_file(struct inotify_event *event, ddb_playlist_t* pl){
    char* basename_tmp = strdup(event->name);

    char* file = basename(basename_tmp);

    if(strstr(ext_list, get_filename_ext(file)) == NULL){
        free(basename_tmp);
        return;
    }
    free(basename_tmp);
    const char* parent_name = g_hash_table_lookup(wd_table,&event->wd);

    char* path = malloc(strlen(parent_name) + 1 + strlen(event->name) + 2);
    sprintf(path,"%s/%s",parent_name,event->name);
    g_hash_table_insert(wait_table,path,NULL);
//    char* basename_tmp = strdup(event->name);
//
//    char* file = basename(basename_tmp);
//
//    if(strstr(ext_list, get_filename_ext(file)) == NULL){
//        free(basename_tmp);
//        return;
//    }
//    free(basename_tmp);
//
//    const char* sql_form = "insert into files(path) values('%s');";
//    const char* parent_name = g_hash_table_lookup(wd_table,&event->wd);
//
//    char* path = malloc(strlen(parent_name) + 1 + strlen(event->name) + 2);
//
//    sprintf(path,"%s/%s",parent_name,event->name);
//
//    char* sql = malloc(strlen(sql_form) + strlen(path) + 2);
//    sprintf(sql,sql_form,path);
//    int rc = sqlite3_exec(db, sql, 0, 0, &err_msg);
//
//    if (rc != SQLITE_OK ) {
//        fprintf(stderr, "SQL error: %s\n", err_msg);
//    }
//
//    deadbeef->pl_lock();
//    deadbeef->plt_add_file2(1,pl,path,NULL,NULL);
////    deadbeef->plt_insert_file2(1,pl,deadbeef->plt_get_last(pl,0),path,NULL,NULL,NULL);
//    deadbeef->pl_unlock();
//
//    free(sql);
//    free(path);
}

void file_close(struct inotify_event *event, ddb_playlist_t* pl){
    const char* parent_name = g_hash_table_lookup(wd_table,&event->wd);

    char* path = malloc(strlen(parent_name) + 1 + strlen(event->name) + 2);
    sprintf(path,"%s/%s",parent_name,event->name);
    printf("%s\n",path);
    if(g_hash_table_contains(wait_table,path)){
        printf("file write end\n");
        g_hash_table_remove(wait_table,path);
        printf("removed from table\n");

        const char* sql_form = "insert into files(path) values('%s');";

        char* sql = malloc(strlen(sql_form) + strlen(path) + 2);
        sprintf(sql,sql_form,path);

        int rc = sqlite3_exec(db, sql, 0, 0, &err_msg);

        if (rc != SQLITE_OK ) {
            fprintf(stderr, "SQL error: %s\n", err_msg);
        }

        deadbeef->pl_lock();
        deadbeef->plt_add_file2(1,pl,path,NULL,NULL);
//    deadbeef->plt_insert_file2(1,pl,deadbeef->plt_get_last(pl,0),path,NULL,NULL,NULL);
        deadbeef->pl_unlock();

        free(path);
        free(sql);
    }
}

void remove_file(struct inotify_event *event, ddb_playlist_t* pl){
    const char* sql_form = "delete from files where path = '%s';";
//    const char* path = event->name;
    const char* parent_name = g_hash_table_lookup(wd_table,&event->wd);
    char* path = malloc(strlen(parent_name) + 1 + strlen(event->name) + 2);

    sprintf(path,"%s/%s",parent_name,event->name);

    printf("%s\n", path);
    char* sql = malloc(strlen(sql_form) + strlen(path) + 2);
    sprintf(sql,sql_form,path);

    int rc = sqlite3_exec(db, sql, 0, 0, &err_msg);

    if (rc != SQLITE_OK ) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
    }

    ddb_playItem_t* playItem = find_by_path(pl,path);
    printf("finded: %p\n", playItem);
    if(playItem == NULL){
        free(sql);
        free(path);
        return;
    }

    deadbeef->pl_lock();
    deadbeef->pl_item_ref(playItem);

    deadbeef->plt_remove_item(pl,playItem);
    printf("removed\n");
//
    deadbeef->pl_item_unref(playItem);
    deadbeef->pl_unlock();

    free(sql);
    free(path);
}

static void watch_thread(ddb_playlist_t* pl){

    int length, i;
    char buffer[BUF_LEN];

    struct pollfd fds[1];

    fds[0].fd = in_fd;
    fds[0].events = POLLIN;

    while(!shutdown)
    {
        printf("test\n");
        i = 0;

        int ret = poll( fds, 1, 10000);

        if(ret == -1){
            printf("error\n");
            break;
        }else if(ret == 0){
            continue;
        }else{
            fds[0].revents = 0;
        }

        length = read( in_fd, buffer, BUF_LEN );


        if ( length < 0 ) {
            perror( "read" );
        }

        while ( i < length ) {
            struct inotify_event *event = ( struct inotify_event * ) &buffer[ i ];
            if ( event->len ) {

                if (event->mask & IN_CREATE) {
                    if (event->mask & IN_ISDIR) {
                        printf("The directory %s was Created.\n", event->name);
                        add_dir(event);
                    }else{
                        printf("The file %s was Created with WD %d - cookie %d\n", event->name, event->wd,
                               event->cookie);
                        add_file(event,pl);
                    }
                }else if ( event->mask & IN_DELETE){
                    if (event->mask & IN_ISDIR) {
                        //TODO удалять wd у далёной дериктории
                        printf("STAB The directory %s was deleted.\n", event->name);
                    }else {
                        printf("The file %s was deleted with WD %d - cookie %d\n", event->name, event->wd,
                               event->cookie);
                        remove_file(event,pl);
                    }
                }else if (event->mask & IN_MOVED_FROM){
//                    printf("IN_MOVED_FROM %s\n", event->name);
                    if (event->mask & IN_ISDIR) {
                        //TODO удалять wd у далёной дериктории
                        printf("STAB The directory %s was deleted.\n", event->name);
                    }else {
                        printf("The file %s was deleted with WD %d - cookie %d\n", event->name, event->wd,
                               event->cookie);
                        remove_file(event,pl);
                    }
                }else if(event->mask & IN_MOVED_TO){
                    printf("IN_MOVED_TO %s\n", event->name);
                    if (event->mask & IN_ISDIR) {
                        printf("The directory %s was Created.\n", event->name);
                        add_dir(event);
                    }else {
                        printf("The file %s was Created with WD %d - cookie %d\n", event->name, event->wd,
                               event->cookie);
                        add_file(event,pl);
                    }
                }else if(event->mask & IN_CLOSE){
                    if(!(event->mask & IN_ISDIR)){
                        file_close(event,pl);
                    }

                }

                i += EVENT_SIZE + event->len;

            }
        }
    }
}

static int connect(){
    ddb_playlist_t* pl = deadbeef->plt_find_by_name("test");
    if(pl == NULL){
        printf("play list not found\n");
        return 0;
    }
    printf("play list found\n");

    deadbeef->plt_add_files_begin(pl,1);

    //find new files
    printf("удалёные файлы\n");
    sqlite3_exec(db,"SELECT path FROM files\n"
                    "    EXCEPT\n"
                    "    SELECT path FROM files_tmp;",remove_callback,pl,&err_msg);
    printf("\n");
    //find deleted files
    printf("новые файлы\n");
    sqlite3_exec(db,"SELECT path FROM files_tmp\n"
                    "    EXCEPT\n"
                    "    SELECT path FROM files;",add_callback,pl,&err_msg);

    deadbeef->plt_add_files_end(pl,1);
    deadbeef->plt_modified(pl);

    watch_thread_ptr = deadbeef->thread_start((void (*)(void *)) watch_thread, pl);

    return 0;
}

static int stop(){

    shutdown = TRUE;
    deadbeef->thread_join(watch_thread_ptr);

    g_hash_table_destroy(wd_table);
    sqlite3_close(db);
    return 0;
}

static DB_misc_t plugin = {
        .plugin = {
                .api_vmajor = 1,
                .api_vminor = 14,
                .id = NULL,
                .name = "watch dir",
                .descr = "auto refresh play list",
                .copyright = "Murloc Knight",
                .website = "https://github.com/KnightMurloc/DeadBeef-X11-Overlay-Plugin-",

                .command = NULL,
                .start = start,
                .stop = stop,
                .connect = connect,
                .disconnect = NULL,
                .exec_cmdline = NULL,
                .get_actions = NULL,
                .message = NULL,
                .configdialog = NULL
        }
};


extern DB_plugin_t *watch_dir_load(DB_functions_t *api) {
    deadbeef = api;
    return &plugin.plugin;
}