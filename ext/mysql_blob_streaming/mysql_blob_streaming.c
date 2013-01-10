#include <ruby.h>
#include <stdlib.h>

#include <mysql.h>
#include <errmsg.h>


// TODO: This method needs to recycle the connection of Mysql2::Client!
static MYSQL * mysql_connection(VALUE obj)
{
    MYSQL *conn;
    conn = mysql_init(NULL);
    if (conn == NULL) {
        printf("Error %u: %s\n", mysql_errno(conn), mysql_error(conn));
        exit(1);
    }

    if (mysql_real_connect(conn, "localhost", "mbs", "mbs", "mysql_blob_streaming", 0, NULL, 0) == NULL) {
        printf("Error %u: %s\n", mysql_errno(conn), mysql_error(conn));
        exit(1);
    } else {
        // printf("Erfolgreich verbunden!!!\n");
    }

    return conn;
}


static MYSQL_STMT * prepare_and_execute_stmt_with_query(MYSQL *conn, char *query)
{
    MYSQL_STMT *stmt = mysql_stmt_init(conn);
    if (stmt == NULL) {
        rb_raise(rb_eRuntimeError, "Could not initialize prepared statement!");
    }

    int prepare_error = mysql_stmt_prepare(stmt, query, strlen(query));
    if (prepare_error) {
        rb_raise(rb_eRuntimeError, "Could not prepare statement! Error Code: %d", prepare_error);
    }

    long nr_params = mysql_stmt_param_count(stmt);
    if (nr_params) {
        rb_raise(rb_eRuntimeError, "Query contains %lu placeholders. 0 are allowed!", nr_params);
    }

    int exec_code = mysql_stmt_execute(stmt);
    if (exec_code) {
        rb_raise(rb_eRuntimeError, "Could not execute statement. MySQL error code: %d", exec_code);
    }

    return stmt;
}


static void store_buffer(MYSQL_STMT *stmt, int offset_index, MYSQL_BIND *bind, int chunk_length, VALUE obj)
{
    int status = mysql_stmt_fetch_column(stmt, bind, 0, offset_index);
    if (status != 0) {
        rb_raise(rb_eRuntimeError, "Fetching column failed");
    }
    if (!*bind[0].is_null) {
        if (bind[0].buffer_type == MYSQL_TYPE_BLOB) {
            rb_funcall(obj, rb_intern("handle_data"), 1, rb_str_new(bind[0].buffer, chunk_length));
        } else {
            rb_raise(rb_eRuntimeError, "wrong buffer_type (must be: MYSQL_TYPE_BLOB): %d",
                    bind[0].buffer_type);
        }
    }
}


static int determine_blob_length(MYSQL_STMT *stmt, MYSQL_BIND *bind)
{
    int original_buffer_length = bind[0].buffer_length;
    bind[0].buffer_length = 0;

    if (mysql_stmt_bind_result(stmt, bind) != 0) {
        rb_raise(rb_eRuntimeError, "determine_blob_length2 Could not determine the blob length: bind failed");
    }
    int status = mysql_stmt_fetch(stmt);
    // MYSQL_DATA_TRUNCATED is returned if MYSQL_REPORT_DATA_TRUNCATION connection option is set
    if (status != 0 && status != MYSQL_DATA_TRUNCATED) {
        rb_raise(rb_eRuntimeError, "determine_blob_length2 Could not determine the blob length: fetch failed");
    }

    bind[0].buffer_length = original_buffer_length;
    return *bind[0].length;
}


static void loop_store_buffer(MYSQL_STMT *stmt, MYSQL_BIND *bind, int buffer_length, int total_blob_length, VALUE obj)
{
    long loops = abs(total_blob_length / buffer_length);
    long i;
    for (i = 0; i < loops; ++i) {
        store_buffer(stmt, i * buffer_length, bind, buffer_length, obj);
    }
    int new_bufflen = total_blob_length % buffer_length;
    if (new_bufflen) {
        store_buffer(stmt, loops * buffer_length, bind, new_bufflen, obj);
    }
}


static MYSQL_BIND * build_result_bind(MYSQL_STMT *stmt, int buffer_length)
{
    MYSQL_BIND *bind = (MYSQL_BIND *)calloc(1, sizeof(MYSQL_BIND));
    bind[0].length = (unsigned long *)malloc(sizeof(unsigned long));
    bind[0].is_null = (my_bool *)malloc(sizeof(my_bool));
    bind[0].buffer_length = buffer_length;
    bind[0].buffer = malloc(buffer_length);

    MYSQL_RES *result_set = mysql_stmt_result_metadata(stmt);
    if ((result_set != NULL) && (mysql_num_fields(result_set) >= 1)) {
        MYSQL_FIELD *columns = mysql_fetch_fields(result_set);
        bind[0].buffer_type = columns[0].type;
    }

    return bind;
}


static void free_result_bind(MYSQL_BIND *bind)
{
    if (bind != NULL) {
        free(bind[0].buffer);
        free(bind->length);
        free(bind->is_null);
        free(bind);
    }
}


static VALUE stmt_fetch_and_write(VALUE obj, VALUE rb_buffer_length, VALUE rb_query)
{
    int buffer_length = FIX2INT(rb_buffer_length);

    if (buffer_length == 0) {
        return 0;
    }
    if (buffer_length < 0) {
        rb_raise(rb_eRuntimeError, "buffer size must be integer >= 0");
    }

    char *query = RSTRING_PTR(rb_query);

    MYSQL *conn = mysql_connection(obj);
    MYSQL_STMT *stmt = prepare_and_execute_stmt_with_query(conn, query);
    MYSQL_BIND *bind = build_result_bind(stmt, buffer_length);

    int total_blob_length = determine_blob_length(stmt, bind);
    loop_store_buffer(stmt, bind, buffer_length, total_blob_length, obj);

    mysql_stmt_close(stmt);
    free_result_bind(bind);
    return Qnil;
}


void Init_mysql_blob_streaming()
{
    VALUE rb_mMysqlBlobStreaming = rb_define_module("MysqlBlobStreaming");
    rb_define_method(rb_mMysqlBlobStreaming, "stream", stmt_fetch_and_write, 2);
}
