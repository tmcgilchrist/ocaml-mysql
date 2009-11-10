/*
 * This module provides access to MySQL from Objective Caml. 
 */

 
#include <stdio.h>              /* sprintf */
#include <string.h> 
#include <stdarg.h>
 
/* OCaml runtime system */

#include <caml/mlvalues.h>
#include <caml/memory.h>
#include <caml/fail.h>
#include <caml/alloc.h>
#include <caml/callback.h>
#include <caml/custom.h>
#include <caml/signals.h>

/* MySQL API */

#include <mysql/mysql.h>
#include <mysql/mysqld_error.h>
#include <mysql/errmsg.h>
/* type 'a option = None | Some of 'a */

#define EXTERNAL                /* dummy to highlight fn's exported to ML */

#ifdef CAML_TEST_GC_SAFE
#include <unistd.h>
#define caml_enter_blocking_section() if (1) { caml_enter_blocking_section(); sleep(1); }
#endif

/* Abstract types 
 *
 * dbd - data base descriptor
 *
 *      header with Abstract_tag
 *      0:      MYSQL*
 *      1:      bool    (open == true, closed == false)
 *
 * res - result returned from query/exec
 *
 *      header with Final_tag
 *      0:      finalization function for 1
 *      1:      MYSQL_RES*
 *
 */

/* macros to access C values stored inside the abstract values */
 
#define DBDmysql(x) ((MYSQL*)(Field(x,1)))
#define DBDopen(x) (Field(x,2))
#define RESval(x) (*(MYSQL_RES**)Data_custom_val(x))

#define STMTval(x) (*(MYSQL_STMT**)Data_custom_val(x))
#define ROWval(x) (*(row_t**)Data_custom_val(x))

static void mysqlfailwith(char *err) Noreturn;
static void mysqlfailmsg(const char *fmt, ...) Noreturn;

static void
mysqlfailwith(char *err) {
  raise_with_string(*caml_named_value("mysql error"), err);
}

static void
mysqlfailmsg(const char *fmt, ...) {
  char buf[1024];
  va_list args;

  va_start(args, fmt);
  vsnprintf(buf, sizeof buf, fmt, args);
  va_end(args);
  
  raise_with_string(*caml_named_value("mysql error"), buf);  

}

#define Val_none Val_int(0)

static inline value
Val_some( value v )
{
    CAMLparam1( v );
    CAMLlocal1( some );
    some = caml_alloc_small(1, 0);
    Field(some, 0) = v;
    CAMLreturn( some );
}

#define Some_val(v) Field(v,0)

static int
int_option(value v)
{
  if (v == Val_none)
    return 0;
  else
    return  Int_val(Some_val(v));
}


/*
 * strdup_option copies a char* from an ML string option value.  Returns
 * NULL for None.
 */

static char*
strdup_option(value v)
{
  if (v == Val_none)
    return (char*) NULL;
  else 
    return strdup(String_val(Some_val(v)));
}

/*
 * val_str_option creates a string option value from a char* (NONE for
 * NULL) -- dual to str_option().
 */
 
static value
val_str_option(const char* s, unsigned long length)
{
  CAMLparam0();
  CAMLlocal1(v);

  if (!s)
    CAMLreturn(Val_none);
  else
  {
    v = caml_alloc_string(length);
    memcpy(String_val(v), s, length);

    CAMLreturn(Val_some(v));
  }
}

/* check_db checks that the data base connection is still open.  The
 * open flag is reset by db_disconnect().
 */

static inline MYSQL*
check_db(value dbd, const char *fun)
{
  if (!Bool_val(DBDopen(dbd))) 
    mysqlfailmsg("Mysql.%s called with closed connection", fun);
  return DBDmysql(dbd);
}


static void
conn_finalize(value dbd)
{
  if (Bool_val(DBDopen(dbd))) 
  {
    MYSQL* db = DBDmysql(dbd);
    caml_enter_blocking_section();
    mysql_close(db);
    caml_leave_blocking_section();
  }
}

/* db_connect opens a data base connection and returns an abstract
 * connection object.
 */
 
EXTERNAL value
db_connect(value args)

{
  CAMLparam1(args);
  CAMLlocal1(res);
  int i = 0;
  char *host      = NULL;
  char *db        = NULL;
  unsigned int port     = 0;
  char *pwd       = NULL;
  char *user      = NULL;
  MYSQL *init;
  MYSQL *mysql;

  init = mysql_init(NULL);
  if (!init) 
  {
    mysqlfailwith("connect failed");
  } 
  else 
  {
    i = 0;
    host      = strdup_option(Field(args,i++));
    db        = strdup_option(Field(args,i++));
    port      = (unsigned int) int_option(Field(args,i++));
    pwd       = strdup_option(Field(args,i++));
    user      = strdup_option(Field(args,i++));

    caml_enter_blocking_section();
    mysql = mysql_real_connect(init ,host ,user
                               ,pwd ,db ,port
                               ,NULL, 0);
    caml_leave_blocking_section();

    free(host); free(db); free(pwd); free(user);

    if (!mysql) 
    {
      mysqlfailwith((char*)mysql_error(init));
    }
    else 
    {
      res = alloc_final(3, conn_finalize, 100,1000);
      Field(res, 1) = (value)mysql;
      Field(res, 2) =  Val_true;
    }
  }
  CAMLreturn(res);
}


EXTERNAL value
db_change_user(value v_dbd, value args) 
{
  char *db;
  char *pwd;
  char *user;
  my_bool ret;
  MYSQL* mysql = check_db(v_dbd,"change_user");

  db        = strdup_option(Field(args,1));
  pwd       = strdup_option(Field(args,3));
  user      = strdup_option(Field(args,4));

  caml_enter_blocking_section();
  ret = mysql_change_user(mysql, user, pwd, db);
  caml_leave_blocking_section();

  free(db); free(pwd); free(user);

  if (ret)
    mysqlfailmsg("Mysql.change_user: %s", mysql_error(mysql));

  return Val_unit;
}

EXTERNAL value
db_list_dbs(value v_dbd, value pattern, value blah)
{
  CAMLparam3(v_dbd, pattern, blah);
  CAMLlocal1(dbs);
  MYSQL* mysql = check_db(v_dbd,"list_dbs");
  char *wild = strdup_option(pattern);
  int n, i;
  MYSQL_RES *res;
  MYSQL_ROW row;

  caml_enter_blocking_section();
  res = mysql_list_dbs(mysql, wild);
  caml_leave_blocking_section();

  free(wild);

  if (!res)
    CAMLreturn(Val_none);

  n = mysql_num_rows(res);

  if (n == 0) 
  {
    mysql_free_result(res);
    CAMLreturn(Val_none);
  }

  dbs = alloc_tuple(n); /* Array */
  i = 0;
  while ((row = mysql_fetch_row(res)) != NULL) 
  {
    Store_field(dbs, i, copy_string(row[0]));
    i++;
  }

  mysql_free_result(res);
  CAMLreturn(Val_some(dbs));
}

EXTERNAL value
db_select_db(value v_dbd, value v_newdb)
{
  CAMLparam2(v_dbd,v_newdb);
  MYSQL* mysql = check_db(v_dbd, "select_db");
  char* newdb = strdup(String_val(v_newdb));
  my_bool ret;
 
  caml_enter_blocking_section();
  ret = mysql_select_db(mysql, newdb);
  caml_leave_blocking_section();

  free(newdb);

  if (ret)
    mysqlfailmsg("Mysql.select_db: %s", mysql_error(mysql));

  CAMLreturn(Val_unit);
}

/*
 * db_disconnect closes a db connection and marks the dbd closed.
 */

EXTERNAL value
db_disconnect(value dbd)
{
  CAMLparam1(dbd);
  MYSQL* db = check_db(dbd,"disconnect");
  caml_enter_blocking_section();
  mysql_close(db);
  caml_leave_blocking_section();
  Field(dbd, 1) = Val_false;
  Field(dbd, 2) = Val_false; /* Mark closed */
  CAMLreturn(Val_unit);
}

EXTERNAL value
db_ping(value dbd)
{
  CAMLparam1(dbd);
  MYSQL* db = check_db(dbd,"ping");

  caml_enter_blocking_section();
  if (mysql_ping(db)) 
  {
    caml_leave_blocking_section();
    mysqlfailmsg("Mysql.ping: %s", mysql_error(db));
  }
  caml_leave_blocking_section();

  CAMLreturn(Val_unit);
}

/*
 * finalize -- this is called when a data base result is garbage
 * collected -- frees memory allocated by MySQL.
 */

static void
res_finalize(value result)
{
  MYSQL_RES *res = RESval(result);
  if (res)
    mysql_free_result(res);
}


struct custom_operations res_ops = {
  "Mysql Query Results",
  res_finalize,
  custom_compare_default,
  custom_hash_default,
  custom_serialize_default,
  custom_deserialize_default
};

/*
 * db_exec -- execute a SQL query or command.  Returns a handle to
 * access the result.
 */

EXTERNAL value
db_exec(value v_dbd, value v_sql)
{
  CAMLparam2(v_dbd, v_sql);
  CAMLlocal1(res);
  MYSQL *mysql = check_db(v_dbd,"exec");
  char* sql = strdup(String_val(v_sql));
  size_t len = caml_string_length(v_sql);
  int ret;

  caml_enter_blocking_section();
  ret = mysql_real_query(mysql, sql, len);
  caml_leave_blocking_section();

  free(sql);

  if (ret)
  {
    mysqlfailmsg("Mysql.exec: %s", mysql_error(mysql));
  }
  else
  {
    res = alloc_custom(&res_ops, sizeof(MYSQL_RES*), 1, 10);
    RESval(res) = mysql_store_result(mysql);
  }

  CAMLreturn(res);
}

/* 
 * db_fetch -- fetch one result tuple, represented as array of string
 * options.  In case a value is Null, the respective value is None. 
 * Returns (Some v) in case there is such a result and None otherwise. 
 * Moves the internal result cursor to the next tuple.
 */

EXTERNAL value
db_fetch (value result)
{
  CAMLparam1(result);
  CAMLlocal2(fields, s);
  unsigned int i, n;
  unsigned long *length;  /* array of long */
  MYSQL_RES *res;
  MYSQL_ROW row;
  
  res = RESval(result);
  if (!res) 
    mysqlfailwith("Mysql.fetch: result did not return fetchable data");
  
  n = mysql_num_fields(res);
  if (n == 0)
    mysqlfailwith("Mysql.fetch: no columns");
  
  row = mysql_fetch_row(res);
  if (!row) 
    CAMLreturn(Val_none);
  
  /* create Some([| f1; f2; .. ;fn |]) */
  
  length = mysql_fetch_lengths(res);      /* length[] */
  fields = alloc_tuple(n);                    /* array */
  for (i=0;i<n;i++) {
    s = val_str_option(row[i], length[i]);
    Store_field(fields, i, s);
  }
  
  CAMLreturn(Val_some(fields));
}

EXTERNAL value
db_to_row(value result, value offset) {
  int64 off = Int64_val(offset);
  MYSQL_RES *res;

  res = RESval(result);
  if (!res) 
    mysqlfailwith("Mysql.to_row: result did not return fetchable data");

  if (off < 0 || off > (int64)mysql_num_rows(res)-1)
    invalid_argument("Mysql.to_row: offset out of range");

  mysql_data_seek(res, off);

  return Val_unit;
}

/*
 * db_status -- returns current status (simplistic)
 */

EXTERNAL value
db_status(value dbd)
{
  CAMLparam1(dbd);
  CAMLreturn(Val_int(mysql_errno(check_db(dbd,"status"))));
}

/*
 * db_errmsg -- returns string option with last error message (None ==
 * no error)
 */

EXTERNAL value
db_errmsg(value dbd)
{
  CAMLparam1(dbd);
  CAMLlocal1(s);
  const char *msg;

  msg = mysql_error(check_db(dbd,"errmsg"));
  if (!msg || msg[0] == '\0')
    msg = (char*) NULL;
  s = val_str_option(msg, msg == (char *) NULL ? 0 : strlen(msg));
  CAMLreturn(s);
}

/*
 * db_escape - takes a string and escape all characters inside which
 * must be escaped inside MySQL strings.  This helps to construct SQL
 * statements easily.  Simply returns the new string including the
 * quotes. 
 */

EXTERNAL value
db_escape(value str)
{
  CAMLparam1(str);
  char *s;
  char *buf;
  int len, esclen;
  CAMLlocal1(res);
        
  s = String_val(str);
  len = string_length(str);
  buf = (char*) stat_alloc(2*len+1);
  esclen = mysql_escape_string(buf,s,len);

  res = alloc_string(esclen);
  memcpy(String_val(res), buf, esclen);
  stat_free(buf);
  
  CAMLreturn(res);
}

/*
 * db_size -- returns the size of the current result (number of rows).
 */

EXTERNAL value
db_size(value result)
{
  CAMLparam1(result);
  MYSQL_RES *res;
  int64 size;

  res = RESval(result);
  if (!res)
    size = 0;
  else
    size = (int64)(mysql_num_rows(res));
  
  CAMLreturn(copy_int64(size));
}

EXTERNAL value
db_affected(value dbd) {
  CAMLparam1(dbd);
  CAMLreturn(copy_int64(mysql_affected_rows(DBDmysql(dbd))));
}

EXTERNAL value
db_insert_id(value dbd) {
  CAMLparam1(dbd);
  CAMLreturn(copy_int64(mysql_insert_id(DBDmysql(dbd))));
}

EXTERNAL value
db_fields(value result)
{
  MYSQL_RES *res;
  long size;

  res = RESval(result);
  if (!res)
    size = 0;
  else
    size = (long)(mysql_num_fields(res));
  
  return Val_long(size);
}

EXTERNAL value
db_client_info(value unit) {
  CAMLparam1(unit);
  CAMLlocal1(info);
  info = copy_string(mysql_get_client_info());
  CAMLreturn(info);
}

EXTERNAL value
db_host_info(value dbd) {
  CAMLparam1(dbd);
  CAMLlocal1(info);
  info = copy_string(mysql_get_host_info(DBDmysql(dbd)));
  CAMLreturn(info);
}

EXTERNAL value
db_server_info(value dbd) {
  CAMLparam1(dbd);
  CAMLlocal1(info);
  info = copy_string(mysql_get_server_info(DBDmysql(dbd)));
  CAMLreturn(info);
}

EXTERNAL value
db_proto_info(value dbd) {
  long info = (long)mysql_get_proto_info(DBDmysql(dbd));
  return Val_long(info);
}


/*
 * type2dbty - maps column types to dbty values which describe the
 * column types in OCaml.
 */

#define INT_TY          0
#define FLOAT_TY        1
#define STRING_TY       2
#define SET_TY          3
#define ENUM_TY         4
#define DATETIME_TY     5
#define DATE_TY         6
#define TIME_TY         7
#define YEAR_TY         8
#define TIMESTAMP_TY    9
#define UNKNOWN_TY      10
#define INT64_TY        11
#define BLOB_TY         12
#define DECIMAL_TY			13

static value
type2dbty (int type)
{
  static struct {int mysql; value caml;} map[] = {
    {FIELD_TYPE_DECIMAL     , Val_long(DECIMAL_TY)},
    {FIELD_TYPE_TINY        , Val_long(INT_TY)},
    {FIELD_TYPE_SHORT       , Val_long(INT_TY)},
    {FIELD_TYPE_LONG        , Val_long(INT_TY)},
    {FIELD_TYPE_FLOAT       , Val_long(FLOAT_TY)},
    {FIELD_TYPE_DOUBLE      , Val_long(FLOAT_TY)},
    {FIELD_TYPE_NULL        , Val_long(STRING_TY)},
    {FIELD_TYPE_TIMESTAMP   , Val_long(TIMESTAMP_TY)},
    {FIELD_TYPE_LONGLONG    , Val_long(INT64_TY)},
    {FIELD_TYPE_INT24       , Val_long(INT_TY)},
    {FIELD_TYPE_DATE        , Val_long(DATE_TY)},
    {FIELD_TYPE_TIME        , Val_long(TIME_TY)},
    {FIELD_TYPE_DATETIME    , Val_long(DATETIME_TY)},
    {FIELD_TYPE_YEAR        , Val_long(YEAR_TY)},
    {FIELD_TYPE_NEWDATE     , Val_long(UNKNOWN_TY)},
    {FIELD_TYPE_ENUM        , Val_long(ENUM_TY)},
    {FIELD_TYPE_SET         , Val_long(SET_TY)},
    {FIELD_TYPE_TINY_BLOB   , Val_long(BLOB_TY)},
    {FIELD_TYPE_MEDIUM_BLOB , Val_long(BLOB_TY)},
    {FIELD_TYPE_LONG_BLOB   , Val_long(BLOB_TY)},
    {FIELD_TYPE_BLOB        , Val_long(BLOB_TY)},
    {FIELD_TYPE_VAR_STRING  , Val_long(STRING_TY)},
    {FIELD_TYPE_STRING      , Val_long(STRING_TY)},
    {-1 /*default*/         , Val_long(UNKNOWN_TY)}
  };
  int i;
  
  /* in principle using bsearch() would be better -- but how can
   * we know that the left side of the map is properly sorted? 
   */

  for (i=0; map[i].mysql != -1 && map[i].mysql != type; i++)
    /* empty */ ;
  
  return map[i].caml;
}

value 
make_field(MYSQL_FIELD *f) {
  CAMLparam0();
  CAMLlocal5(out, data, name, table, def);

  name = copy_string(f->name);

  if (f->table)
    table = val_str_option(f->table, strlen(f->table));
  else
    table = Val_none;

  if (f->def)
    def = val_str_option(f->def, strlen(f->def));
  else
    def = Val_none;

  data = alloc_small(7, 0);
  Field(data, 0) = name;
  Field(data, 1) = table;
  Field(data, 2) = def;
  Field(data, 3) = type2dbty(f->type);
  Field(data, 4) = Val_long(f->max_length);
  Field(data, 5) = Val_long(f->flags);
  Field(data, 6) = Val_long(f->decimals);

  CAMLreturn(data);
}

EXTERNAL value
db_fetch_field(value result) {
  CAMLparam1(result);
  CAMLlocal2(field, out);
  MYSQL_FIELD *f;
  MYSQL_RES *res = RESval(result);

  if (!res)
    CAMLreturn(Val_none);

  f = mysql_fetch_field(res);
  if (!f)
    CAMLreturn(Val_none);

  field = make_field(f);
  CAMLreturn(Val_some(field));
}

EXTERNAL value
db_fetch_field_dir(value result, value pos) {
  CAMLparam2(result, pos);
  CAMLlocal2(field, out);
  MYSQL_FIELD *f;
  MYSQL_RES *res = RESval(result);

  if (!res)
    CAMLreturn(Val_none);

  f = mysql_fetch_field_direct(res, Long_val(pos));
  if (!f)
    CAMLreturn(Val_none);

  field = make_field(f);
  CAMLreturn(Val_some(field));
}


EXTERNAL value
db_fetch_fields(value result) {
  CAMLparam1(result);
  CAMLlocal1(fields);
  MYSQL_RES *res = RESval(result);
  MYSQL_FIELD *f;
  int i, n;

  n = mysql_num_fields(res);

  if (n == 0)
    CAMLreturn(Val_none);

  f = mysql_fetch_fields(res);

  fields = alloc_tuple(n);

  for (i = 0; i < n; i++) {
    Store_field(fields, i, make_field(f+i));
  }

  CAMLreturn(Val_some(fields));
}

static void
check_stmt(MYSQL_STMT* stmt, char *fun)
{
  if (!stmt) 
    mysqlfailmsg("Mysql.Prepared.%s called with closed statement", fun);
}

static void
stmt_finalize(value v_stmt)
{
  MYSQL_STMT* stmt = STMTval(v_stmt);
  if (!stmt) return;
  caml_enter_blocking_section();
  mysql_stmt_close(stmt);
  caml_leave_blocking_section();
  STMTval(v_stmt) = (MYSQL_STMT*)NULL;
}

struct custom_operations stmt_ops = {
  "Mysql Prepared Statement",
  stmt_finalize,
  custom_compare_default,
  custom_hash_default,
  custom_serialize_default,
  custom_deserialize_default
};


EXTERNAL value
caml_mysql_stmt_prepare(value v_dbd, value v_sql)
{
  CAMLparam2(v_dbd,v_sql);
  CAMLlocal1(res);
  MYSQL* db = check_db(v_dbd, "Prepared.create");
  char* sql_c = strdup(String_val(v_sql));
  if (!sql_c)
    mysqlfailwith("Mysql.Prepared.create : strdup");
  caml_enter_blocking_section();
  MYSQL_STMT* stmt = mysql_stmt_init(db);
  if (!stmt)
  {
    free(sql_c);
    caml_leave_blocking_section();
    mysqlfailwith("Mysql.Prepared.create : mysql_stmt_init");
  }
  int ret = mysql_stmt_prepare(stmt, sql_c, strlen(sql_c));
  free(sql_c);
  if (ret)
  { 
    mysql_stmt_close(stmt);
    caml_leave_blocking_section();
    mysqlfailmsg("Mysql.Prepared.create : mysql_stmt_prepare = %i. Query : %s",ret,String_val(v_sql));
  }
  caml_leave_blocking_section();
  res = alloc_custom(&stmt_ops, sizeof(MYSQL_STMT*), 0, 1);
  STMTval(res) = stmt;
  CAMLreturn(res);
}

EXTERNAL value
caml_mysql_stmt_close(value v_stmt)
{
  CAMLparam1(v_stmt);
  MYSQL_STMT* stmt = STMTval(v_stmt);
  check_stmt(stmt,"close");
  caml_enter_blocking_section();
  my_bool ret = mysql_stmt_close(stmt);
  caml_leave_blocking_section();
  if (ret)
    mysqlfailwith("mysql_stmt_close");
  STMTval(v_stmt) = (MYSQL_STMT*)NULL;
  CAMLreturn(Val_unit);
}

typedef struct row_t_tag
{
  size_t count;
  MYSQL_STMT* stmt; /* not owned */

  MYSQL_BIND* bind;
  unsigned long* length;
  my_bool* error;
  my_bool* is_null;
} row_t;

row_t* create_row(MYSQL_STMT* stmt, size_t count)
{
  row_t* row = malloc(sizeof(row_t));
  if (row)
  {
    row->stmt = stmt;
    row->count = count;
    row->bind = calloc(count,sizeof(MYSQL_BIND));
    row->error = calloc(count,sizeof(my_bool));
    row->length = calloc(count,sizeof(unsigned long));
    row->is_null = calloc(count,sizeof(my_bool));
  }
  return row;
}

void set_param(row_t *r, char* str, size_t len, int index)
{
  MYSQL_BIND* bind = &r->bind[index];

  r->length[index] = len;
  bind->length = &r->length[index];
  bind->buffer_length = len;
  bind->buffer_type = MYSQL_TYPE_STRING;
  bind->buffer = (void*)str;
}

void bind_result(row_t* r, int index)
{
  MYSQL_BIND* bind = &r->bind[index];

  bind->buffer_type = MYSQL_TYPE_STRING;
  bind->buffer = 0;
  bind->buffer_length = 0;
  bind->is_null = &r->is_null[index];
  bind->length = &r->length[index];
  bind->error = &r->error[index];
}

value get_column(row_t* r, int index)
{
  CAMLparam0();
  CAMLlocal1(str);
  unsigned long length = r->length[index];
  MYSQL_BIND* bind = &r->bind[index];

  if (*bind->is_null) CAMLreturn(Val_none);
  if (0 == length)
  {
    str = caml_copy_string("");
  }
  else
  {
    str = caml_alloc_string(length);
    bind->buffer = String_val(str);
    bind->buffer_length = length;
    mysql_stmt_fetch_column(r->stmt, bind, index, 0);
    bind->buffer = 0; // reset binding
    bind->buffer_length = 0;
  }

  CAMLreturn(Val_some(str));
}

void destroy_row(row_t* r)
{
  if (r)
  {
    free(r->bind);
    free(r->error);
    free(r->length);
    free(r->is_null);
    free(r);
  }
}

static void
stmt_result_finalize(value result)
{
  row_t *row = ROWval(result);
  destroy_row(row);
}

struct custom_operations stmt_result_ops = {
  "Mysql Prepared Statement Results",
  stmt_result_finalize,
  custom_compare_default,
  custom_hash_default,
  custom_serialize_default,
  custom_deserialize_default
};

EXTERNAL value
caml_mysql_stmt_execute(value v_stmt, value v_params)
{
  CAMLparam2(v_stmt,v_params);
  CAMLlocal2(res,v);
  MYSQL_STMT* stmt = STMTval(v_stmt);
  check_stmt(stmt,"execute");
  unsigned int i = 0;
  unsigned int len = Wosize_val(v_params);
  int err = 0;
  char* bufs[256];
  if (len != mysql_stmt_param_count(stmt))
    mysqlfailmsg("Prepared.execute : Got %i parameters, but expected %u", len, mysql_stmt_param_count(stmt));
  if (len > 256)
    mysqlfailwith("Prepared.execute : too many parameters");
  row_t* row = create_row(stmt, len);
  if (!row)
    mysqlfailwith("Prepared.execute : create_row for params");
  for (i = 0; i < len; i++)
  {
    v = Field(v_params,i);
    bufs[i] = malloc(caml_string_length(v));
    memcpy(bufs[i],String_val(v),caml_string_length(v));
    set_param(row,bufs[i],caml_string_length(v),i);
  }
  err = mysql_stmt_bind_param(stmt, row->bind);
  if (err)
  {
    destroy_row(row);
    for (i = 0; i < len; i++) free(bufs[i]);
    mysqlfailmsg("Prepared.execute : mysql_stmt_bind_param = %i",err);
  }
  caml_enter_blocking_section();
  err = mysql_stmt_execute(stmt);
  caml_leave_blocking_section();
  if (err)
  {
    destroy_row(row);
    for (i = 0; i < len; i++) free(bufs[i]);
    mysqlfailmsg("Prepared.execute : mysql_stmt_execute = %i, %s",err,mysql_stmt_error(stmt));
  }
  destroy_row(row);
  for (i = 0; i < len; i++) free(bufs[i]);

  len = mysql_stmt_field_count(stmt);
  row = create_row(stmt, len);
  if (!row)
    mysqlfailwith("Prepared.execute : create_row for results");
  if (len)
  {
    for (i = 0; i < len; i++)
    {
      bind_result(row,i);
    }
    if (mysql_stmt_bind_result(stmt, row->bind))
    {
      destroy_row(row);
      mysqlfailwith("Prepared.execute : mysql_stmt_bind_result");
    }
  }
  /* FIXME tweak used/max? */
  res = alloc_custom(&stmt_result_ops, sizeof(row_t*), 1, 10);
  ROWval(res) = row;
  CAMLreturn(res);
}

EXTERNAL value
caml_mysql_stmt_fetch(value result)
{
  CAMLparam1(result);
  CAMLlocal1(arr);
  unsigned int i = 0;
  row_t* r = ROWval(result);
  check_stmt(r->stmt,"fetch");
  caml_enter_blocking_section();
  int res = mysql_stmt_fetch(r->stmt);
  caml_leave_blocking_section();
  if (0 != res && MYSQL_DATA_TRUNCATED != res) CAMLreturn(Val_none);
  arr = caml_alloc(r->count,0);
  for (i = 0; i < r->count; i++)
  {
    Store_field(arr,i,get_column(r,i));
  }
  CAMLreturn(Val_some(arr));
}

EXTERNAL value
caml_mysql_stmt_affected(value stmt) 
{
  CAMLparam1(stmt);
  check_stmt(STMTval(stmt),"affected");
  CAMLreturn(copy_int64(mysql_stmt_affected_rows(STMTval(stmt))));
}

EXTERNAL value
caml_mysql_stmt_insert_id(value stmt) 
{
  CAMLparam1(stmt);
  check_stmt(STMTval(stmt),"insert_id");
  CAMLreturn(copy_int64(mysql_stmt_insert_id(STMTval(stmt))));
}

EXTERNAL value
caml_mysql_stmt_status(value stmt)
{
  CAMLparam1(stmt);
  check_stmt(STMTval(stmt), "status");
  CAMLreturn(Val_int(mysql_stmt_errno(STMTval(stmt))));
}

