/*
  Connection handling code

  See the accompanying LICENSE file.
*/

/**

.. _connections:

Connections to a database
*************************

A :class:`Connection` encapsulates access to a database.  You then use
:class:`cursors <Cursor>` to issue queries against the database.

You can have multiple :class:`Connections <Connection>` open against
the same database in the same process, across threads and in other
processes.

*/

/* CALLBACK INFO */

/* details of a registered function passed as user data to sqlite3_create_function */
typedef struct FunctionCBInfo
{
  PyObject_HEAD const char *name; /* utf8 function name */
  PyObject *scalarfunc;           /* the function to call for stepping */
  PyObject *aggregatefactory;     /* factory for aggregate functions */
} FunctionCBInfo;

/* a particular aggregate function instance used as sqlite3_aggregate_context */
typedef struct _aggregatefunctioncontext
{
  PyObject *aggvalue;  /* the aggregation value passed as first parameter */
  PyObject *stepfunc;  /* step function */
  PyObject *finalfunc; /* final function */
} aggregatefunctioncontext;

/* CONNECTION TYPE */

struct Connection
{
  PyObject_HEAD
      sqlite3 *db; /* the actual database connection */
  unsigned inuse;  /* track if we are in use preventing concurrent thread mangling */

  struct StatementCache *stmtcache; /* prepared statement cache */

  PyObject *dependents; /* tracking cursors & blobs etc as weakrefs belonging to this connection */

  /* registered hooks/handlers (NULL or callable) */
  PyObject *busyhandler;
  PyObject *rollbackhook;
  PyObject *profile;
  PyObject *updatehook;
  PyObject *commithook;
  PyObject *walhook;
  PyObject *progresshandler;
  PyObject *authorizer;
  PyObject *collationneeded;
  PyObject *exectrace;
  PyObject *rowtrace;

  /* if we are using one of our VFS since sqlite doesn't reference count them */
  PyObject *vfs;

  /* used for nested with (contextmanager) statements */
  long savepointlevel;

  /* informational attributes */
  PyObject *open_flags;
  PyObject *open_vfs;

  /* weak reference support */
  PyObject *weakreflist;
};

typedef struct Connection Connection;

static PyTypeObject ConnectionType;

typedef struct _vtableinfo
{
  PyObject *datasource;   /* object with create/connect methods */
  Connection *connection; /* the Connection this is registered against so we don't
             have to have a global table mapping sqlite3_db* to
             Connection* */
} vtableinfo;

/* forward declarations */
struct APSWBlob;
static void APSWBlob_init(struct APSWBlob *self, Connection *connection, sqlite3_blob *blob);
static PyTypeObject APSWBlobType;

struct APSWBackup;
static void APSWBackup_init(struct APSWBackup *self, Connection *dest, Connection *source, sqlite3_backup *backup);
static PyTypeObject APSWBackupType;

static PyTypeObject APSWCursorType;

struct ZeroBlobBind;
static PyTypeObject ZeroBlobBindType;

static void
FunctionCBInfo_dealloc(FunctionCBInfo *self)
{
  if (self->name)
    PyMem_Free((void *)(self->name));
  Py_CLEAR(self->scalarfunc);
  Py_CLEAR(self->aggregatefactory);
  Py_TYPE(self)->tp_free((PyObject *)self);
}

/** .. class:: Connection


  This object wraps a `sqlite3 pointer
  <https://sqlite.org/c3ref/sqlite3.html>`_.
*/

/* CONNECTION CODE */

static void
Connection_internal_cleanup(Connection *self)
{
  Py_CLEAR(self->busyhandler);
  Py_CLEAR(self->rollbackhook);
  Py_CLEAR(self->profile);
  Py_CLEAR(self->updatehook);
  Py_CLEAR(self->commithook);
  Py_CLEAR(self->walhook);
  Py_CLEAR(self->progresshandler);
  Py_CLEAR(self->authorizer);
  Py_CLEAR(self->collationneeded);
  Py_CLEAR(self->exectrace);
  Py_CLEAR(self->rowtrace);
  Py_CLEAR(self->vfs);
  Py_CLEAR(self->open_flags);
  Py_CLEAR(self->open_vfs);
}

static void
Connection_remove_dependent(Connection *self, PyObject *o)
{
  /* in addition to removing the dependent, we also remove any dead
     weakrefs */
  Py_ssize_t i;

  for (i = 0; i < PyList_GET_SIZE(self->dependents);)
  {
    PyObject *wr = PyList_GET_ITEM(self->dependents, i);
    PyObject *wo = PyWeakref_GetObject(wr);
    if (wo == o || wo == Py_None)
    {
      PyList_SetSlice(self->dependents, i, i + 1, NULL);
      if (wo == Py_None)
        continue;
      else
        return;
    }
    i++;
  }
}

static int
Connection_close_internal(Connection *self, int force)
{
  int res;
  PyObject *etype, *eval, *etb;

  if (force == 2)
    PyErr_Fetch(&etype, &eval, &etb);

  /* close out dependents by repeatedly processing first item until
     list is empty.  note that closing an item will cause the list to
     be perturbed as a side effect */
  while (PyList_GET_SIZE(self->dependents))
  {
    PyObject *closeres, *item, *wr = PyList_GET_ITEM(self->dependents, 0);
    item = PyWeakref_GetObject(wr);
    if (item == Py_None)
    {
      Connection_remove_dependent(self, item);
      continue;
    }

    closeres = Call_PythonMethodV(item, "close", 1, "(i)", !!force);
    Py_XDECREF(closeres);
    if (!closeres)
    {
      assert(PyErr_Occurred());
      if (force == 2)
        apsw_write_unraiseable(NULL);
      else
        return 1;
    }
  }

  if (self->stmtcache)
    statementcache_free(self->stmtcache);
  self->stmtcache = 0;

  PYSQLITE_VOID_CALL(
      APSW_FAULT_INJECT(ConnectionCloseFail, res = sqlite3_close(self->db), res = SQLITE_IOERR));

  self->db = 0;

  if (res != SQLITE_OK)
  {
    SET_EXC(res, NULL);
    if (force == 2)
    {
      PyErr_Format(ExcConnectionNotClosed,
                   "apsw.Connection at address %p. The destructor "
                   "has encountered an error %d closing the connection, but cannot raise an exception.",
                   self, res);
      apsw_write_unraiseable(NULL);
    }
  }

  Connection_internal_cleanup(self);

  if (PyErr_Occurred())
  {
    assert(force != 2);
    AddTraceBackHere(__FILE__, __LINE__, "Connection.close", NULL);
    return 1;
  }

  if (force == 2)
    PyErr_Restore(etype, eval, etb);
  return 0;
}

/** .. method:: close(force: bool = False) -> None

  Closes the database.  If there are any outstanding :class:`cursors
  <Cursor>`, :class:`blobs <blob>` or :class:`backups <backup>` then
  they are closed too.  It is normally not necessary to call this
  method as the database is automatically closed when there are no
  more references.  It is ok to call the method multiple times.

  If your user defined functions or collations have direct or indirect
  references to the Connection then it won't be automatically garbage
  collected because of circular referencing that can't be
  automatically broken.  Calling *close* will free all those objects
  and what they reference.

  SQLite is designed to survive power failures at even the most
  awkward moments.  Consequently it doesn't matter if it is closed
  when the process is exited, or even if the exit is graceful or
  abrupt.  In the worst case of having a transaction in progress, that
  transaction will be rolled back by the next program to open the
  database, reverting the database to a know good state.

  If *force* is *True* then any exceptions are ignored.

   -* sqlite3_close
*/

/* Closes cursors and blobs belonging to this connection */
static PyObject *
Connection_close(Connection *self, PyObject *args, PyObject *kwds)
{
  int force = 0;

  CHECK_USE(NULL);

  assert(!PyErr_Occurred());
  {
    static char *kwlist[] = {"force", NULL};
    Connection_close_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O&:" Connection_close_USAGE, kwlist, argcheck_bool, &force))
      return NULL;
  }
  if (Connection_close_internal(self, force))
  {
    assert(PyErr_Occurred());
    return NULL;
  }

  Py_RETURN_NONE;
}

static void
Connection_dealloc(Connection *self)
{
  APSW_CLEAR_WEAKREFS;

  Connection_close_internal(self, 2);

  /* Our dependents all hold a refcount on us, so they must have all
     released before this destructor could be called */
  assert(PyList_GET_SIZE(self->dependents) == 0);
  Py_CLEAR(self->dependents);

  Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
Connection_new(PyTypeObject *type, PyObject *Py_UNUSED(args), PyObject *Py_UNUSED(kwds))
{
  Connection *self;

  self = (Connection *)type->tp_alloc(type, 0);
  if (self != NULL)
  {
    self->db = 0;
    self->inuse = 0;
    self->dependents = PyList_New(0);
    self->stmtcache = 0;
    self->busyhandler = 0;
    self->rollbackhook = 0;
    self->profile = 0;
    self->updatehook = 0;
    self->commithook = 0;
    self->walhook = 0;
    self->progresshandler = 0;
    self->authorizer = 0;
    self->collationneeded = 0;
    self->exectrace = 0;
    self->rowtrace = 0;
    self->vfs = 0;
    self->savepointlevel = 0;
    self->open_flags = 0;
    self->open_vfs = 0;
    self->weakreflist = 0;
  }

  return (PyObject *)self;
}

/** .. method:: __init__(filename: str, flags: int = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, vfs: Optional[str] = None, statementcachesize: int = 100)

  Opens the named database.  You can use ``:memory:`` to get a private temporary
  in-memory database that is not shared with any other connections.

  :param flags: One or more of the `open flags <https://sqlite.org/c3ref/c_open_autoproxy.html>`_ orred together
  :param vfs: The name of the `vfs <https://sqlite.org/c3ref/vfs.html>`_ to use.  If :const:`None` then the default
     vfs will be used.

  :param statementcachesize: Use zero to disable the statement cache,
    or a number larger than the total distinct SQL statements you
    execute frequently.

  -* sqlite3_open_v2

  .. seealso::

    * :attr:`apsw.connection_hooks`
    * :ref:`statementcache`
    * :ref:`vfs`

*/
/* forward declaration so we can tell if it is one of ours */
static int apswvfs_xAccess(sqlite3_vfs *vfs, const char *zName, int flags, int *pResOut);

static int
Connection_init(Connection *self, PyObject *args, PyObject *kwds)
{
  PyObject *hooks = NULL, *hook = NULL, *iterator = NULL, *hookargs = NULL, *hookresult = NULL;
  const char *filename = NULL;
  int res = 0;
  int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
  const char *vfs = 0;
  int statementcachesize = 100;
  sqlite3_vfs *vfsused = 0;

  {
    static char *kwlist[] = {"filename", "flags", "vfs", "statementcachesize", NULL};
    Connection_init_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|izi:" Connection_init_USAGE, kwlist, &filename, &flags, &vfs, &statementcachesize))
      return -1;
  }
  flags |= SQLITE_OPEN_EXRESCODE;

  /* clamp cache size */
  if (statementcachesize < 0)
    statementcachesize = 0;
  if (statementcachesize > 16384)
    statementcachesize = 16384;

  /* Technically there is a race condition as a vfs of the same name
     could be registered between our find and the open starting.
     Don't do that!  We also have to manage the error message thread
     safety manually as self->db is null on entry. */
  PYSQLITE_VOID_CALL(
      vfsused = sqlite3_vfs_find(vfs); res = sqlite3_open_v2(filename, &self->db, flags, vfs); if (res != SQLITE_OK) apsw_set_errmsg(sqlite3_errmsg(self->db)););
  SET_EXC(res, self->db); /* nb sqlite3_open always allocates the db even on error */

  if (res != SQLITE_OK)
    goto pyexception;

  if (vfsused && vfsused->xAccess == apswvfs_xAccess)
  {
    PyObject *pyvfsused = (PyObject *)(vfsused->pAppData);
    Py_INCREF(pyvfsused);
    self->vfs = pyvfsused;
  }

  /* record information */
  self->open_flags = PyLong_FromLong(flags);
  if (vfsused)
    self->open_vfs = convertutf8string(vfsused->zName);

  /* get detailed error codes */
  PYSQLITE_VOID_CALL(sqlite3_extended_result_codes(self->db, 1));

  /* call connection hooks */
  hooks = PyObject_GetAttrString(apswmodule, "connection_hooks");
  if (!hooks)
    goto pyexception;

  hookargs = Py_BuildValue("(O)", self);
  if (!hookargs)
    goto pyexception;

  iterator = PyObject_GetIter(hooks);
  if (!iterator)
  {
    AddTraceBackHere(__FILE__, __LINE__, "Connection.__init__", "{s: O}", "connection_hooks", OBJ(hooks));
    goto pyexception;
  }

  self->stmtcache = statementcache_init(self->db, statementcachesize);
  if (!self->stmtcache)
    goto pyexception;

  while ((hook = PyIter_Next(iterator)))
  {
    hookresult = PyObject_CallObject(hook, hookargs);
    if (!hookresult)
      goto pyexception;
    Py_DECREF(hook);
    hook = NULL;
    Py_DECREF(hookresult);
  }

  if (!PyErr_Occurred())
  {
    res = 0;
    goto finally;
  }

pyexception:
  /* clean up db since it is useless - no need for user to call close */
  assert(PyErr_Occurred());
  res = -1;
  Connection_close_internal(self, 2);
  assert(PyErr_Occurred());

finally:
  Py_XDECREF(hookargs);
  Py_XDECREF(iterator);
  Py_XDECREF(hooks);
  Py_XDECREF(hook);
  assert(PyErr_Occurred() || res == 0);
  return res;
}

/** .. method:: blobopen(database: str, table: str, column: str, rowid: int, writeable: bool)  -> Blob

   Opens a blob for :ref:`incremental I/O <blobio>`.

   :param database: Name of the database.  This will be ``main`` for
     the main connection and the name you specified for `attached
     <https://sqlite.org/lang_attach.html>`_ databases.
   :param table: The name of the table
   :param column: The name of the column
   :param rowid: The id that uniquely identifies the row.
   :param writeable: If True then you can read and write the blob.  If False then you can only read it.

   :rtype: :class:`blob`

   .. seealso::

     * :ref:`Blob I/O example <example-blobio>`
     * `SQLite row ids <https://sqlite.org/autoinc.html>`_

   -* sqlite3_blob_open
*/
static PyObject *
Connection_blobopen(Connection *self, PyObject *args, PyObject *kwds)
{
  struct APSWBlob *apswblob = 0;
  sqlite3_blob *blob = 0;
  const char *database, *table, *column;
  long long rowid;
  int writeable = 0;
  int res;
  PyObject *weakref;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  {
    static char *kwlist[] = {"database", "table", "column", "rowid", "writeable", NULL};
    Connection_blobopen_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "sssLO&:" Connection_blobopen_USAGE, kwlist, &database, &table, &column, &rowid, argcheck_bool, &writeable))
      return NULL;
  }
  PYSQLITE_CON_CALL(res = sqlite3_blob_open(self->db, database, table, column, rowid, writeable, &blob));

  SET_EXC(res, self->db);
  if (res != SQLITE_OK)
    return NULL;

  APSW_FAULT_INJECT(BlobAllocFails, apswblob = PyObject_New(struct APSWBlob, &APSWBlobType), (PyErr_NoMemory(), apswblob = NULL));
  if (!apswblob)
  {
    PYSQLITE_CON_CALL(sqlite3_blob_close(blob));
    return NULL;
  }

  APSWBlob_init(apswblob, self, blob);
  weakref = PyWeakref_NewRef((PyObject *)apswblob, NULL);
  PyList_Append(self->dependents, weakref);
  Py_DECREF(weakref);
  return (PyObject *)apswblob;
}

/** .. method:: backup(databasename: str, sourceconnection: Connection, sourcedatabasename: str)  -> Backup

   Opens a :ref:`backup object <Backup>`.  All data will be copied from source
   database to this database.

   :param databasename: Name of the database.  This will be ``main`` for
     the main connection and the name you specified for `attached
     <https://sqlite.org/lang_attach.html>`_ databases.
   :param sourceconnection: The :class:`Connection` to copy a database from.
   :param sourcedatabasename: Name of the database in the source (eg ``main``).

   :rtype: :class:`backup`

   .. seealso::

     * :ref:`Backup`

   -* sqlite3_backup_init
*/
static PyObject *
Connection_backup(Connection *self, PyObject *args, PyObject *kwds)
{
  struct APSWBackup *apswbackup = 0;
  sqlite3_backup *backup = 0;
  int res = -123456; /* stupid compiler */
  PyObject *result = NULL;
  PyObject *weakref = NULL;
  Connection *sourceconnection = NULL;
  const char *databasename = NULL;
  const char *sourcedatabasename = NULL;
  int isetsourceinuse = 0;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  /* gc dependents removing dead items */
  Connection_remove_dependent(self, NULL);

  /* self (destination) can't be used if there are outstanding blobs, cursors or backups */
  if (PyList_GET_SIZE(self->dependents))
  {
    PyObject *args = NULL, *etype, *evalue, *etb;

    APSW_FAULT_INJECT(BackupTupleFails, args = PyTuple_New(2), args = PyErr_NoMemory());
    if (!args)
      goto thisfinally;
    PyTuple_SET_ITEM(args, 0, PyUnicode_FromString("The destination database has outstanding objects open on it.  They must all be closed for the backup to proceed (otherwise corruption would be possible.)"));
    PyTuple_SET_ITEM(args, 1, self->dependents);
    Py_INCREF(self->dependents);

    PyErr_SetObject(ExcThreadingViolation, args);

    PyErr_Fetch(&etype, &evalue, &etb);
    PyErr_NormalizeException(&etype, &evalue, &etb);
    PyErr_Restore(etype, evalue, etb);

  thisfinally:
    Py_XDECREF(args);
    goto finally;
  }

  {
    static char *kwlist[] = {"databasename", "sourceconnection", "sourcedatabasename", NULL};
    Connection_backup_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "sO!s:" Connection_backup_USAGE, kwlist, &databasename, &ConnectionType, &sourceconnection, &sourcedatabasename))
      return NULL;
  }
  if (!sourceconnection->db)
  {
    PyErr_Format(PyExc_ValueError, "source connection is closed!");
    goto finally;
  }

  if (sourceconnection->inuse)
  {
    PyErr_Format(ExcThreadingViolation, "source connection is in concurrent use in another thread");
    goto finally;
  }

  if (sourceconnection->db == self->db)
  {
    PyErr_Format(PyExc_ValueError, "source and destination are the same which sqlite3_backup doesn't allow");
    goto finally;
  }

  sourceconnection->inuse = 1;
  isetsourceinuse = 1;

  APSW_FAULT_INJECT(BackupInitFails,
                    PYSQLITE_CON_CALL((backup = sqlite3_backup_init(self->db, databasename, sourceconnection->db, sourcedatabasename),
                                       res = backup ? SQLITE_OK : sqlite3_extended_errcode(self->db))),
                    res = SQLITE_NOMEM);

  if (res)
  {
    SET_EXC(res, self->db);
    goto finally;
  }

  APSW_FAULT_INJECT(BackupNewFails,
                    apswbackup = PyObject_New(struct APSWBackup, &APSWBackupType),
                    apswbackup = (struct APSWBackup *)PyErr_NoMemory());
  if (!apswbackup)
    goto finally;

  APSWBackup_init(apswbackup, self, sourceconnection, backup);
  Py_INCREF(self);
  Py_INCREF(sourceconnection);
  backup = NULL;

  /* add to dependent lists */
  APSW_FAULT_INJECT(BackupDependent1, weakref = PyWeakref_NewRef((PyObject *)apswbackup, NULL), weakref = PyErr_NoMemory());
  if (!weakref)
    goto finally;
  APSW_FAULT_INJECT(BackupDependent2, res = PyList_Append(self->dependents, weakref), (PyErr_NoMemory(), res = -1));
  if (res)
    goto finally;
  Py_DECREF(weakref);
  APSW_FAULT_INJECT(BackupDependent3, weakref = PyWeakref_NewRef((PyObject *)apswbackup, NULL), weakref = PyErr_NoMemory());
  if (!weakref)
    goto finally;
  APSW_FAULT_INJECT(BackupDependent4, res = PyList_Append(sourceconnection->dependents, weakref), (PyErr_NoMemory(), res = -1));
  if (res)
    goto finally;
  Py_DECREF(weakref);
  weakref = 0;

  result = (PyObject *)apswbackup;
  apswbackup = NULL;

finally:
  /* check errors occurred vs result */
  assert(result ? (PyErr_Occurred() == NULL) : (PyErr_Occurred() != NULL));
  assert(result ? (backup == NULL) : 1);
  if (backup)
    PYSQLITE_VOID_CALL(sqlite3_backup_finish(backup));

  Py_XDECREF((PyObject *)apswbackup);
  Py_XDECREF(weakref);

  /* if inuse is set then we must be returning result */
  assert((self->inuse) ? (!!result) : (result == NULL));
  assert(result ? (self->inuse) : (!self->inuse));
  if (isetsourceinuse)
    sourceconnection->inuse = 0;
  return result;
}

/** .. method:: cursor() -> Cursor

  Creates a new :class:`Cursor` object on this database.

  :rtype: :class:`Cursor`
*/
static PyObject *
Connection_cursor(Connection *self)
{
  struct APSWCursor *cursor = NULL;
  PyObject *weakref;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  APSW_FAULT_INJECT(CursorAllocFails, cursor = (struct APSWCursor *)PyObject_CallFunction((PyObject *)&APSWCursorType, "O", self), cursor = (struct APSWCursor *)PyErr_NoMemory());
  if (!cursor)
    return NULL;

  weakref = PyWeakref_NewRef((PyObject *)cursor, NULL);
  PyList_Append(self->dependents, weakref);
  Py_DECREF(weakref);

  return (PyObject *)cursor;
}

/** .. method:: setbusytimeout(milliseconds: int) -> None

  If the database is locked such as when another connection is making
  changes, SQLite will keep retrying.  This sets the maximum amount of
  time SQLite will keep retrying before giving up.  If the database is
  still busy then :class:`apsw.BusyError` will be returned.

  :param milliseconds: Maximum thousandths of a second to wait.

  If you previously called :meth:`~Connection.setbusyhandler` then
  calling this overrides that.

  .. seealso::

     * :meth:`Connection.setbusyhandler`
     * :ref:`Busy handling <busyhandling>`

  -* sqlite3_busy_timeout
*/
static PyObject *
Connection_setbusytimeout(Connection *self, PyObject *args, PyObject *kwds)
{
  int milliseconds = 0;
  int res;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  {
    static char *kwlist[] = {"milliseconds", NULL};
    Connection_setbusytimeout_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "i:" Connection_setbusytimeout_USAGE, kwlist, &milliseconds))
      return NULL;
  }
  PYSQLITE_CON_CALL(res = sqlite3_busy_timeout(self->db, milliseconds));
  SET_EXC(res, self->db);
  if (res != SQLITE_OK)
    return NULL;

  /* free any explicit busyhandler we may have had */
  Py_XDECREF(self->busyhandler);
  self->busyhandler = 0;

  Py_RETURN_NONE;
}

/** .. method:: changes() -> int

  Returns the number of database rows that were changed (or inserted
  or deleted) by the most recently completed INSERT, UPDATE, or DELETE
  statement.

  -* sqlite3_changes64
*/
static PyObject *
Connection_changes(Connection *self)
{
  sqlite3_int64 changes;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  changes = sqlite3_changes64(self->db);

  /* verify 64 bit values convert fully */
  APSW_FAULT_INJECT(ConnectionChanges64, , changes = ((sqlite3_int64)1000000000) * 7 * 3);

  return PyLong_FromLongLong(changes);
}

/** .. method:: totalchanges() -> int

  Returns the total number of database rows that have be modified,
  inserted, or deleted since the database connection was opened.

  -* sqlite3_total_changes64
*/
static PyObject *
Connection_totalchanges(Connection *self)
{
  sqlite3_int64 changes;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  changes = sqlite3_total_changes64(self->db);
  return PyLong_FromLongLong(changes);
}

/** .. method:: getautocommit() -> bool

  Returns if the Connection is in auto commit mode (ie not in a transaction).

  -* sqlite3_get_autocommit
*/
static PyObject *
Connection_getautocommit(Connection *self)
{
  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);
  if (sqlite3_get_autocommit(self->db))
    Py_RETURN_TRUE;
  Py_RETURN_FALSE;
}

/** .. method:: db_names() -> List[str]

 Returns the list of database names.  For example the first database
 is named 'main', the next 'temp', and the rest with the name provided
 in `ATTACH <https://www.sqlite.org/lang_attach.html>`__

 -* sqlite3_db_name
*/
static PyObject *
Connection_db_names(Connection *self)
{
  PyObject *res = NULL, *str = NULL;
  int i;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  sqlite3_mutex_enter(sqlite3_db_mutex(self->db));

  APSW_FAULT_INJECT(dbnamesnolist, res = PyList_New(0), res = PyErr_NoMemory());
  if (!res)
    goto error;

  for (i = 0; i < APSW_INT32_MAX; i++)
  {
    int appendres;

    const char *s = sqlite3_db_name(self->db, i); /* Doesn't need PYSQLITE_CALL */
    if (!s)
      break;
    APSW_FAULT_INJECT(dbnamestrfail, str = convertutf8string(s), str = PyErr_NoMemory());
    if (!str)
      goto error;
    APSW_FAULT_INJECT(dbnamesappendfail, appendres = PyList_Append(res, str), (PyErr_NoMemory(), appendres = -1));
    if (0 != appendres)
      goto error;
    Py_CLEAR(str);
  }

  sqlite3_mutex_leave(sqlite3_db_mutex(self->db));
  return res;
error:
  sqlite3_mutex_leave(sqlite3_db_mutex(self->db));
  assert(PyErr_Occurred());
  Py_XDECREF(res);
  Py_XDECREF(str);

  return NULL;
}

/** .. method:: last_insert_rowid() -> int

  Returns the integer key of the most recent insert in the database.

  -* sqlite3_last_insert_rowid
*/
static PyObject *
Connection_last_insert_rowid(Connection *self)
{
  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  return PyLong_FromLongLong(sqlite3_last_insert_rowid(self->db));
}

/** .. method:: set_last_insert_rowid(rowid: int) -> None

  Sets the value calls to :meth:`last_insert_rowid` will return.

  -* sqlite3_set_last_insert_rowid
*/
static PyObject *
Connection_set_last_insert_rowid(Connection *self, PyObject *args, PyObject *kwds)
{
  sqlite3_int64 rowid;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  {
    static char *kwlist[] = {"rowid", NULL};
    Connection_set_last_insert_rowid_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "L:" Connection_set_last_insert_rowid_USAGE, kwlist, &rowid))
      return NULL;
  }

  PYSQLITE_VOID_CALL(sqlite3_set_last_insert_rowid(self->db, rowid));

  Py_RETURN_NONE;
}

/** .. method:: interrupt() -> None

  Causes any pending operations on the database to abort at the
  earliest opportunity. You can call this from any thread.  For
  example you may have a long running query when the user presses the
  stop button in your user interface.  :exc:`InterruptError`
  will be raised in the query that got interrupted.

  -* sqlite3_interrupt
*/
static PyObject *
Connection_interrupt(Connection *self)
{
  CHECK_CLOSED(self, NULL);

  sqlite3_interrupt(self->db); /* no return value */
  Py_RETURN_NONE;
}

/** .. method:: limit(id: int, newval: int = -1) -> int

  If called with one parameter then the current limit for that *id* is
  returned.  If called with two then the limit is set to *newval*.


  :param id: One of the `runtime limit ids <https://sqlite.org/c3ref/c_limit_attached.html>`_
  :param newval: The new limit.  This is a 32 bit signed integer even on 64 bit platforms.

  :returns: The limit in place on entry to the call.

  -* sqlite3_limit

  .. seealso::

    * :ref:`Example <example-limit>`

*/
static PyObject *
Connection_limit(Connection *self, PyObject *args, PyObject *kwds)
{
  int newval = -1, res, id;
  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);
  {
    static char *kwlist[] = {"id", "newval", NULL};
    Connection_limit_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "i|i:" Connection_limit_USAGE, kwlist, &id, &newval))
      return NULL;
  }
  res = sqlite3_limit(self->db, id, newval);

  return PyLong_FromLong(res);
}

static void
updatecb(void *context, int updatetype, char const *databasename, char const *tablename, sqlite3_int64 rowid)
{
  /* The hook returns void. That makes it impossible for us to
     abort immediately due to an error in the callback */

  PyGILState_STATE gilstate;
  PyObject *retval = NULL;
  Connection *self = (Connection *)context;

  assert(self);
  assert(self->updatehook);
  assert(self->updatehook != Py_None);

  gilstate = PyGILState_Ensure();

  if (PyErr_Occurred())
    goto finally; /* abort hook due to outstanding exception */

  retval = PyObject_CallFunction(self->updatehook, "(iO&O&L)", updatetype, convertutf8string, databasename, convertutf8string, tablename, rowid);

finally:
  Py_XDECREF(retval);
  PyGILState_Release(gilstate);
}

/** .. method:: setupdatehook(callable: Optional[Callable[[int, str, str, int], None]]) -> None

  Calls *callable* whenever a row is updated, deleted or inserted.  If
  *callable* is :const:`None` then any existing update hook is
  unregistered.  The update hook cannot make changes to the database while
  the query is still executing, but can record them for later use or
  apply them in a different connection.

  The update hook is called with 4 parameters:

    type (int)
      :const:`SQLITE_INSERT`, :const:`SQLITE_DELETE` or :const:`SQLITE_UPDATE`
    database name (string)
      This is ``main`` for the database or the name specified in
      `ATTACH <https://sqlite.org/lang_attach.html>`_
    table name (string)
      The table on which the update happened
    rowid (64 bit integer)
      The affected row

  .. seealso::

      * :ref:`Example <example-updatehook>`

  -* sqlite3_update_hook
*/
static PyObject *
Connection_setupdatehook(Connection *self, PyObject *args, PyObject *kwds)
{
  /* sqlite3_update_hook doesn't return an error code */
  PyObject *callable;
  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  {
    static char *kwlist[] = {"callable", NULL};
    Connection_setupdatehook_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&:" Connection_setupdatehook_USAGE, kwlist, argcheck_Optional_Callable, &callable))
      return NULL;
  }
  if (!callable)
  {
    PYSQLITE_VOID_CALL(sqlite3_update_hook(self->db, NULL, NULL));
  }
  else
  {
    PYSQLITE_VOID_CALL(sqlite3_update_hook(self->db, updatecb, self));
    Py_INCREF(callable);
  }

  Py_XDECREF(self->updatehook);
  self->updatehook = callable;

  Py_RETURN_NONE;
}

static void
rollbackhookcb(void *context)
{
  /* The hook returns void. That makes it impossible for us to
     abort immediately due to an error in the callback */

  PyGILState_STATE gilstate;
  PyObject *retval = NULL;
  Connection *self = (Connection *)context;

  assert(self);
  assert(self->rollbackhook);
  assert(self->rollbackhook != Py_None);

  gilstate = PyGILState_Ensure();

  APSW_FAULT_INJECT(RollbackHookExistingError, , PyErr_NoMemory());

  if (PyErr_Occurred())
    goto finally; /* abort hook due to outstanding exception */

  retval = PyObject_CallObject(self->rollbackhook, NULL);

finally:
  Py_XDECREF(retval);
  PyGILState_Release(gilstate);
}

/** .. method:: setrollbackhook(callable: Optional[Callable[[], None]]) -> None

  Sets a callable which is invoked during a rollback.  If *callable*
  is :const:`None` then any existing rollback hook is unregistered.

  The *callable* is called with no parameters and the return value is ignored.

  -* sqlite3_rollback_hook
*/
static PyObject *
Connection_setrollbackhook(Connection *self, PyObject *args, PyObject *kwds)
{
  /* sqlite3_rollback_hook doesn't return an error code */
  PyObject *callable;
  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  {
    static char *kwlist[] = {"callable", NULL};
    Connection_setrollbackhook_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&:" Connection_setrollbackhook_USAGE, kwlist, argcheck_Optional_Callable, &callable))
      return NULL;
  }

  if (!callable)
  {
    PYSQLITE_VOID_CALL(sqlite3_rollback_hook(self->db, NULL, NULL));
  }
  else
  {
    PYSQLITE_VOID_CALL(sqlite3_rollback_hook(self->db, rollbackhookcb, self));
    Py_INCREF(callable);
  }

  Py_XDECREF(self->rollbackhook);
  self->rollbackhook = callable;

  Py_RETURN_NONE;
}

static void
profilecb(void *context, const char *statement, sqlite_uint64 runtime)
{
  /* The hook returns void. That makes it impossible for us to
     abort immediately due to an error in the callback */

  PyGILState_STATE gilstate;
  PyObject *retval = NULL;
  Connection *self = (Connection *)context;

  assert(self);
  assert(self->profile);
  assert(self->profile != Py_None);

  gilstate = PyGILState_Ensure();

  if (PyErr_Occurred())
    goto finally; /* abort hook due to outstanding exception */

  retval = PyObject_CallFunction(self->profile, "(O&K)", convertutf8string, statement, runtime);

finally:
  Py_XDECREF(retval);
  PyGILState_Release(gilstate);
}

/** .. method:: setprofile(callable: Optional[Callable[[str, int], None]]) -> None

  Sets a callable which is invoked at the end of execution of each
  statement and passed the statement string and how long it took to
  execute. (The execution time is in nanoseconds.) Note that it is
  called only on completion. If for example you do a ``SELECT`` and
  only read the first result, then you won't reach the end of the
  statement.

  -* sqlite3_profile
*/

static PyObject *
Connection_setprofile(Connection *self, PyObject *args, PyObject *kwds)
{
  /* sqlite3_profile doesn't return an error code */
  PyObject *callable;
  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  {
    static char *kwlist[] = {"callable", NULL};
    Connection_setprofile_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&:" Connection_setprofile_USAGE, kwlist, argcheck_Optional_Callable, &callable))
      return NULL;
  }
  if (!callable)
  {
    PYSQLITE_VOID_CALL(sqlite3_profile(self->db, NULL, NULL));
    callable = NULL;
  }
  else
  {
    PYSQLITE_VOID_CALL(sqlite3_profile(self->db, profilecb, self));
    Py_INCREF(callable);
  }

  Py_XDECREF(self->profile);
  self->profile = callable;

  Py_RETURN_NONE;
}

static int
commithookcb(void *context)
{
  /* The hook returns 0 for commit to go ahead and non-zero to abort
     commit (turn into a rollback). We return non-zero for errors */

  PyGILState_STATE gilstate;
  PyObject *retval = NULL;
  int ok = 1; /* error state */
  Connection *self = (Connection *)context;

  assert(self);
  assert(self->commithook);
  assert(self->commithook != Py_None);

  gilstate = PyGILState_Ensure();

  APSW_FAULT_INJECT(CommitHookExistingError, , PyErr_NoMemory());

  if (PyErr_Occurred())
    goto finally; /* abort hook due to outstanding exception */

  retval = PyObject_CallObject(self->commithook, NULL);

  if (!retval)
    goto finally; /* abort hook due to exception */

  ok = PyObject_IsTrue(retval);
  assert(ok == -1 || ok == 0 || ok == 1);
  if (ok == -1)
  {
    ok = 1;
    goto finally; /* abort due to exception in return value */
  }

finally:
  Py_XDECREF(retval);
  PyGILState_Release(gilstate);
  return ok;
}

/** .. method:: setcommithook(callable: Optional[Callable[[], None]]) -> None

  *callable* will be called just before a commit.  It should return
  False for the commit to go ahead and True for it to be turned
  into a rollback. In the case of an exception in your callable, a
  True (ie rollback) value is returned.  Pass None to unregister
  the existing hook.

  .. seealso::

    * :ref:`Example <example-commithook>`

  -* sqlite3_commit_hook

*/
static PyObject *
Connection_setcommithook(Connection *self, PyObject *args, PyObject *kwds)
{
  /* sqlite3_commit_hook doesn't return an error code */
  PyObject *callable;
  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  {
    static char *kwlist[] = {"callable", NULL};
    Connection_setcommithook_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&:" Connection_setcommithook_USAGE, kwlist, argcheck_Optional_Callable, &callable))
      return NULL;
  }
  if (!callable)
  {
    PYSQLITE_VOID_CALL(sqlite3_commit_hook(self->db, NULL, NULL));
    goto finally;
  }

  PYSQLITE_VOID_CALL(sqlite3_commit_hook(self->db, commithookcb, self));

  Py_INCREF(callable);

finally:

  Py_XDECREF(self->commithook);
  self->commithook = callable;

  Py_RETURN_NONE;
}

static int
walhookcb(void *context, sqlite3 *db, const char *dbname, int npages)
{
  PyGILState_STATE gilstate;
  PyObject *retval = NULL;
  int code = SQLITE_ERROR;
  Connection *self = (Connection *)context;

  assert(self);
  assert(self->walhook);
  assert(self->walhook != Py_None);
  assert(self->db == db);

  gilstate = PyGILState_Ensure();

  retval = PyObject_CallFunction(self->walhook, "(OO&i)", self, convertutf8string, dbname, npages);
  if (!retval)
  {
    assert(PyErr_Occurred());
    AddTraceBackHere(__FILE__, __LINE__, "walhookcallback", "{s: O, s: s, s: i}",
                     "Connection", self,
                     "dbname", dbname,
                     "npages", npages);
    goto finally;
  }
  if (!PyLong_Check(retval))
  {
    PyErr_Format(PyExc_TypeError, "wal hook must return a number");
    AddTraceBackHere(__FILE__, __LINE__, "walhookcallback", "{s: O, s: s, s: i, s: O}",
                     "Connection", self,
                     "dbname", dbname,
                     "npages", npages,
                     "retval", OBJ(retval));
    goto finally;
  }
  code = (int)PyLong_AsLong(retval);

finally:
  Py_XDECREF(retval);
  PyGILState_Release(gilstate);
  return code;
}

/** .. method:: setwalhook(callable: Optional[Callable[[Connection, str, int], int]]) -> None

 *callable* will be called just after data is committed in :ref:`wal`
 mode.  It should return :const:`SQLITE_OK` or an error code.  The
 callback is called with 3 parameters:

   * The Connection
   * The database name (eg "main" or the name of an attached database)
   * The number of pages in the wal log

 You can pass in None in order to unregister an existing hook.

 -* sqlite3_wal_hook

*/

static PyObject *
Connection_setwalhook(Connection *self, PyObject *args, PyObject *kwds)
{
  PyObject *callable;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  {
    static char *kwlist[] = {"callable", NULL};
    Connection_setwalhook_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&:" Connection_setwalhook_USAGE, kwlist, argcheck_Optional_Callable, &callable))
      return NULL;
  }

  if (!callable)
  {
    PYSQLITE_VOID_CALL(sqlite3_wal_hook(self->db, NULL, NULL));
  }
  else
  {
    PYSQLITE_VOID_CALL(sqlite3_wal_hook(self->db, walhookcb, self));
    Py_INCREF(callable);
  }
  Py_XDECREF(self->walhook);
  self->walhook = callable;

  Py_RETURN_NONE;
}

static int
progresshandlercb(void *context)
{
  /* The hook returns 0 for continue and non-zero to abort (rollback).
     We return non-zero for errors */

  PyGILState_STATE gilstate;
  PyObject *retval = NULL;
  int ok = 1; /* error state */
  Connection *self = (Connection *)context;

  assert(self);
  assert(self->progresshandler);

  gilstate = PyGILState_Ensure();

  retval = PyObject_CallObject(self->progresshandler, NULL);

  if (!retval)
    goto finally; /* abort due to exception */

  ok = PyObject_IsTrue(retval);

  assert(ok == -1 || ok == 0 || ok == 1);
  if (ok == -1)
  {
    ok = 1;
    goto finally; /* abort due to exception in result */
  }

finally:
  Py_XDECREF(retval);

  PyGILState_Release(gilstate);
  return ok;
}

/** .. method:: setprogresshandler(callable: Optional[Callable[[], bool]], nsteps: int = 20) -> None

  Sets a callable which is invoked every *nsteps* SQLite
  inststructions. The callable should return True to abort
  or False to continue. (If there is an error in your Python *callable*
  then True/abort will be returned).

  .. seealso::

     * :ref:`Example <example-progress-handler>`

  -* sqlite3_progress_handler
*/

static PyObject *
Connection_setprogresshandler(Connection *self, PyObject *args, PyObject *kwds)
{
  /* sqlite3_progress_handler doesn't return an error code */
  int nsteps = 20;
  PyObject *callable = NULL;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);
  {
    static char *kwlist[] = {"callable", "nsteps", NULL};
    Connection_setprogresshandler_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&|i:" Connection_setprogresshandler_USAGE, kwlist, argcheck_Optional_Callable, &callable, &nsteps))
      return NULL;
  }
  if (!callable)
  {
    PYSQLITE_VOID_CALL(sqlite3_progress_handler(self->db, 0, NULL, NULL));
  }
  else
  {
    PYSQLITE_VOID_CALL(sqlite3_progress_handler(self->db, nsteps, progresshandlercb, self));
    Py_INCREF(callable);
  }

  Py_XDECREF(self->progresshandler);
  self->progresshandler = callable;

  Py_RETURN_NONE;
}

static int
authorizercb(void *context, int operation, const char *paramone, const char *paramtwo, const char *databasename, const char *triggerview)
{
  /* should return one of SQLITE_OK, SQLITE_DENY, or
     SQLITE_IGNORE. (0, 1 or 2 respectively) */

  PyGILState_STATE gilstate;
  PyObject *retval = NULL;
  int result = SQLITE_DENY; /* default to deny */
  Connection *self = (Connection *)context;

  assert(self);
  assert(self->authorizer);
  assert(self->authorizer != Py_None);

  gilstate = PyGILState_Ensure();

  APSW_FAULT_INJECT(AuthorizerExistingError, , PyErr_NoMemory());

  if (PyErr_Occurred())
    goto finally; /* abort due to earlier exception */

  retval = PyObject_CallFunction(self->authorizer, "(iO&O&O&O&)", operation, convertutf8string, paramone,
                                 convertutf8string, paramtwo, convertutf8string, databasename,
                                 convertutf8string, triggerview);

  if (!retval)
    goto finally; /* abort due to exception */

  if (PyLong_Check(retval))
  {
    result = PyLong_AsLong(retval);
    goto haveval;
  }

  PyErr_Format(PyExc_TypeError, "Authorizer must return a number");
  AddTraceBackHere(__FILE__, __LINE__, "authorizer callback", "{s: i, s: s:, s: s, s: s}",
                   "operation", operation, "paramone", paramone, "paramtwo", paramtwo,
                   "databasename", databasename, "triggerview", triggerview);

haveval:
  if (PyErr_Occurred())
    result = SQLITE_DENY;

finally:
  Py_XDECREF(retval);

  PyGILState_Release(gilstate);
  return result;
}

/** .. method:: setauthorizer(callable: Optional[Callable[[int, Optional[str], Optional[str], Optional[str], Optional[str]], int]]) -> None

  While `preparing <https://sqlite.org/c3ref/prepare.html>`_
  statements, SQLite will call any defined authorizer to see if a
  particular action is ok to be part of the statement.

  Typical usage would be if you are running user supplied SQL and want
  to prevent harmful operations.  You should also
  set the :class:`statementcachesize <Connection>` to zero.

  The authorizer callback has 5 parameters:

    * An `operation code <https://sqlite.org/c3ref/c_alter_table.html>`_
    * A string (or None) dependent on the operation `(listed as 3rd) <https://sqlite.org/c3ref/c_alter_table.html>`_
    * A string (or None) dependent on the operation `(listed as 4th) <https://sqlite.org/c3ref/c_alter_table.html>`_
    * A string name of the database (or None)
    * Name of the innermost trigger or view doing the access (or None)

  The authorizer callback should return one of :const:`SQLITE_OK`,
  :const:`SQLITE_DENY` or :const:`SQLITE_IGNORE`.
  (:const:`SQLITE_DENY` is returned if there is an error in your
  Python code).

  Passing None unregisters the existing authorizer.

  .. seealso::

    * :ref:`Example <authorizer-example>`
    * :ref:`statementcache`

  -* sqlite3_set_authorizer
*/

static PyObject *
Connection_setauthorizer(Connection *self, PyObject *args, PyObject *kwds)
{
  int res;
  PyObject *callable;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  {
    static char *kwlist[] = {"callable", NULL};
    Connection_setauthorizer_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&:" Connection_setauthorizer_USAGE, kwlist, argcheck_Optional_Callable, &callable))
      return NULL;
  }

  if (!callable)
  {
    APSW_FAULT_INJECT(SetAuthorizerNullFail,
                      PYSQLITE_CON_CALL(res = sqlite3_set_authorizer(self->db, NULL, NULL)),
                      res = SQLITE_IOERR);
    if (res != SQLITE_OK)
    {
      SET_EXC(res, self->db);
      return NULL;
    }
    goto finally;
  }

  APSW_FAULT_INJECT(SetAuthorizerFail,
                    PYSQLITE_CON_CALL(res = sqlite3_set_authorizer(self->db, authorizercb, self)),
                    res = SQLITE_IOERR);

  if (res != SQLITE_OK)
  {
    SET_EXC(res, self->db);
    return NULL;
  }

  Py_INCREF(callable);

finally:
  Py_XDECREF(self->authorizer);
  self->authorizer = callable;

  Py_RETURN_NONE;
}

static void
autovacuum_pages_cleanup(void *callable)
{
  PyGILState_STATE gilstate;

  gilstate = PyGILState_Ensure();
  Py_DECREF((PyObject *)callable);
  PyGILState_Release(gilstate);
}

#define AVPCB_CALL "(O&III)"
#define AVPCB_TB "{s: O, s: s:, s: I, s: I, s: I, s: O}"

static unsigned int
autovacuum_pages_cb(void *callable, const char *schema, unsigned int nPages, unsigned int nFreePages, unsigned int nBytesPerPage)
{
  PyGILState_STATE gilstate;
  PyObject *retval = NULL;
  long res = 0;
  gilstate = PyGILState_Ensure();

  retval = PyObject_CallFunction((PyObject *)callable, AVPCB_CALL, convertutf8string, schema, nPages, nFreePages, nBytesPerPage);

  if (retval && PyLong_Check(retval))
  {
    res = PyLong_AsLong(retval);
    goto finally;
  }

  if (retval)
    PyErr_Format(PyExc_TypeError, "autovacuum_pages callback must return a number not %R", retval ? retval : Py_None);
  AddTraceBackHere(__FILE__, __LINE__, "autovacuum_pages_callback", AVPCB_TB,
                   "callback", OBJ((PyObject *)callable), "schema", schema, "nPages", nPages, "nFreePages", nFreePages, "nBytesPerPage", nBytesPerPage,
                   "result", OBJ(retval));

finally:
  Py_XDECREF(retval);
  PyGILState_Release(gilstate);
  return (int)res;
}

#undef AVPCB_CAL
#undef AVPCB_TB

/** .. method:: autovacuum_pages(callable: Optional[Callable[[str, int, int, int], int]]) -> None

  Calls `callable` to find out how many pages to autovacuum.  The callback has 4 parameters:

  * Database name: str (eg "main")
  * Database pages: int (how many pages make up the database now)
  * Free pages: int (how many pages could be freed)
  * Page size: int (page size in bytes)

  Return how many pages should be freed.  Values less than zero or more than the free pages are
  treated as zero or free page count.  On error zero is returned.

  READ THE NOTE IN THE SQLITE DOCUMENTATION.  Calling into SQLite can result in crashes, corrupt
  databases or worse.

  -* sqlite3_autovacuum_pages
*/
static PyObject *
Connection_autovacuum_pages(Connection *self, PyObject *args, PyObject *kwds)
{
  int res;
  PyObject *callable;
  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  {
    static char *kwlist[] = {"callable", NULL};
    Connection_autovacuum_pages_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&:" Connection_autovacuum_pages_USAGE, kwlist, argcheck_Optional_Callable, &callable))
      return NULL;
  }
  if (!callable)
  {
    PYSQLITE_CON_CALL(res = sqlite3_autovacuum_pages(self->db, NULL, NULL, NULL));
  }
  else
  {
    APSW_FAULT_INJECT(AutovacuumPagesFails,
                      PYSQLITE_CON_CALL(res = sqlite3_autovacuum_pages(self->db, autovacuum_pages_cb, callable, autovacuum_pages_cleanup)),
                      res = SQLITE_NOMEM);
    if (res == SQLITE_OK)
      Py_INCREF(callable);
  }

  if (res != SQLITE_OK)
  {
    SET_EXC(res, self->db);
    return NULL;
  }
  Py_RETURN_NONE;
}

static void
collationneeded_cb(void *pAux, sqlite3 *Py_UNUSED(db), int eTextRep, const char *name)
{
  PyObject *res = NULL, *pyname = NULL;
  Connection *self = (Connection *)pAux;
  PyGILState_STATE gilstate = PyGILState_Ensure();

  assert(self->collationneeded);
  if (!self->collationneeded)
    goto finally;
  if (PyErr_Occurred())
    goto finally;
  pyname = convertutf8string(name);
  if (pyname)
    res = PyObject_CallFunction(self->collationneeded, "(OO)", self, pyname);
  if (!pyname || !res)
    AddTraceBackHere(__FILE__, __LINE__, "collationneeded callback", "{s: O, s: i, s: s}",
                     "Connection", self, "eTextRep", eTextRep, "name", name);
  Py_XDECREF(res);

finally:
  Py_XDECREF(pyname);
  PyGILState_Release(gilstate);
}

/** .. method:: collationneeded(callable: Optional[Callable[[Connection, str], None]]) -> None

  *callable* will be called if a statement requires a `collation
  <http://en.wikipedia.org/wiki/Collation>`_ that hasn't been
  registered. Your callable will be passed two parameters. The first
  is the connection object. The second is the name of the
  collation. If you have the collation code available then call
  :meth:`Connection.createcollation`.

  This is useful for creating collations on demand.  For example you
  may include the `locale <http://en.wikipedia.org/wiki/Locale>`_ in
  the collation name, but since there are thousands of locales in
  popular use it would not be useful to :meth:`prereigster
  <Connection.createcollation>` them all.  Using
  :meth:`~Connection.collationneeded` tells you when you need to
  register them.

  .. seealso::

    * :meth:`~Connection.createcollation`

  -* sqlite3_collation_needed
*/
static PyObject *
Connection_collationneeded(Connection *self, PyObject *args, PyObject *kwds)
{
  int res;
  PyObject *callable;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  {
    static char *kwlist[] = {"callable", NULL};
    Connection_collationneeded_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&:" Connection_collationneeded_USAGE, kwlist, argcheck_Optional_Callable, &callable))
      return NULL;
  }

  if (!callable)
  {
    APSW_FAULT_INJECT(CollationNeededNullFail,
                      PYSQLITE_CON_CALL(res = sqlite3_collation_needed(self->db, NULL, NULL)),
                      res = SQLITE_IOERR);
    if (res != SQLITE_OK)
    {
      SET_EXC(res, self->db);
      return NULL;
    }
    callable = NULL;
    goto finally;
  }

  APSW_FAULT_INJECT(CollationNeededFail,
                    PYSQLITE_CON_CALL(res = sqlite3_collation_needed(self->db, self, collationneeded_cb)),
                    res = SQLITE_IOERR);
  if (res != SQLITE_OK)
  {
    SET_EXC(res, self->db);
    return NULL;
  }

  Py_INCREF(callable);

finally:
  Py_XDECREF(self->collationneeded);
  self->collationneeded = callable;

  Py_RETURN_NONE;
}

static int
busyhandlercb(void *context, int ncall)
{
  /* Return zero for caller to get SQLITE_BUSY error. We default to
     zero in case of error. */

  PyGILState_STATE gilstate;
  PyObject *retval;
  int result = 0; /* default to fail with SQLITE_BUSY */
  Connection *self = (Connection *)context;

  assert(self);
  assert(self->busyhandler);

  gilstate = PyGILState_Ensure();

  retval = PyObject_CallFunction(self->busyhandler, "i", ncall);

  if (!retval)
    goto finally; /* abort due to exception */

  result = PyObject_IsTrue(retval);
  assert(result == -1 || result == 0 || result == 1);
  Py_DECREF(retval);

  if (result == -1)
  {
    result = 0;
    goto finally; /* abort due to exception converting retval */
  }

finally:
  PyGILState_Release(gilstate);
  return result;
}

/** .. method:: setbusyhandler(callable: Optional[Callable[[int], bool]]) -> None

   Sets the busy handler to callable. callable will be called with one
   integer argument which is the number of prior calls to the busy
   callback for the same lock. If the busy callback returns False,
   then SQLite returns :const:`SQLITE_BUSY` to the calling code. If
   the callback returns True, then SQLite tries to open the table
   again and the cycle repeats.

   If you previously called :meth:`~Connection.setbusytimeout` then
   calling this overrides that.

   Passing None unregisters the existing handler.

   .. seealso::

     * :meth:`Connection.setbusytimeout`
     * :ref:`Busy handling <busyhandling>`

   -* sqlite3_busy_handler

*/
static PyObject *
Connection_setbusyhandler(Connection *self, PyObject *args, PyObject *kwds)
{
  int res = SQLITE_OK;
  PyObject *callable;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  {
    static char *kwlist[] = {"callable", NULL};
    Connection_setbusyhandler_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&:" Connection_setbusyhandler_USAGE, kwlist, argcheck_Optional_Callable, &callable))
      return NULL;
  }

  if (!callable)
  {
    APSW_FAULT_INJECT(SetBusyHandlerNullFail,
                      PYSQLITE_CON_CALL(res = sqlite3_busy_handler(self->db, NULL, NULL)),
                      res = SQLITE_IOERR);
    if (res != SQLITE_OK)
    {
      SET_EXC(res, self->db);
      return NULL;
    }
    goto finally;
  }

  APSW_FAULT_INJECT(SetBusyHandlerFail,
                    PYSQLITE_CON_CALL(res = sqlite3_busy_handler(self->db, busyhandlercb, self)),
                    res = SQLITE_IOERR);
  if (res != SQLITE_OK)
  {
    SET_EXC(res, self->db);
    return NULL;
  }

  Py_INCREF(callable);

finally:
  Py_XDECREF(self->busyhandler);
  self->busyhandler = callable;

  Py_RETURN_NONE;
}

#ifndef SQLITE_OMIT_DESERIALZE
/** .. method:: serialize(name: str) -> bytes

  Returns a memory copy of the database. *name* is **"main"** for the
  main database, **"temp"** for the temporary database etc.

  The memory copy is the same as if the database was backed up to
  disk.

  If the database name doesn't exist or is empty, then None is
  returned, not an exception (this is SQLite's behaviour).

   .. seealso::

     * :meth:`Connection.deserialize`

   -* sqlite3_serialize

*/
static PyObject *
Connection_serialize(Connection *self, PyObject *args, PyObject *kwds)
{
  PyObject *pyres = NULL;
  const char *name;
  sqlite3_int64 size = 0;
  unsigned char *serialization = NULL;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  {
    static char *kwlist[] = {"name", NULL};
    Connection_serialize_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s:" Connection_serialize_USAGE, kwlist, &name))
      return NULL;
  }

  /* sqlite3_serialize does not use the same error pattern as other
  SQLite APIs.  I originally coded this as though error codes/strings
  were done behind the scenes.  However that turns out not to be the
  case so this code can't do anything about errors.  See commit
  history for prior attempt */

  INUSE_CALL(_PYSQLITE_CALL_V(serialization = sqlite3_serialize(self->db, name, &size, 0)));

  if (serialization)
    pyres = PyBytes_FromStringAndSize((char *)serialization, size);

  sqlite3_free(serialization);
  if (pyres)
    return pyres;
  if (PyErr_Occurred())
    return NULL;
  Py_RETURN_NONE;
}

/** .. method:: deserialize(name: str, contents: bytes) -> None

   Replaces the named database with an in-memory copy of *contents*.
   *name* is **"main"** for the main database, **"temp"** for the
   temporary database etc.

   The resulting database is in-memory, read-write, and the memory is
   owned, resized, and freed by SQLite.

   .. seealso::

     * :meth:`Connection.serialize`

   -* sqlite3_deserialize

*/
static PyObject *
Connection_deserialize(Connection *self, PyObject *args, PyObject *kwds)
{
  const char *name = NULL;
  Py_buffer contents;

  char *newcontents = NULL;
  int res = SQLITE_OK;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  {
    static char *kwlist[] = {"name", "contents", NULL};
    Connection_deserialize_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "sy*:" Connection_deserialize_USAGE, kwlist, &name, &contents))
      return NULL;
  }

  APSW_FAULT_INJECT(DeserializeMallocFail, newcontents = sqlite3_malloc64(contents.len), newcontents = NULL);
  if (newcontents)
    memcpy(newcontents, contents.buf, contents.len);
  else
  {
    res = SQLITE_NOMEM;
    PyErr_NoMemory();
  }

  if (res == SQLITE_OK)
    PYSQLITE_CON_CALL(res = sqlite3_deserialize(self->db, name, (unsigned char *)newcontents, contents.len, contents.len, SQLITE_DESERIALIZE_RESIZEABLE | SQLITE_DESERIALIZE_FREEONCLOSE));
  SET_EXC(res, self->db);

  if (res != SQLITE_OK)
    return NULL;
  Py_RETURN_NONE;
}
#endif /* SQLITE_OMIT_DESERIALZE */

/** .. method:: enableloadextension(enable: bool) -> None

  Enables/disables `extension loading
  <https://sqlite.org/cvstrac/wiki/wiki?p=LoadableExtensions>`_
  which is disabled by default.

  :param enable: If True then extension loading is enabled, else it is disabled.

  -* sqlite3_enable_load_extension

  .. seealso::

    * :meth:`~Connection.loadextension`
*/

static PyObject *
Connection_enableloadextension(Connection *self, PyObject *args, PyObject *kwds)
{
  int enable, res;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  {
    static char *kwlist[] = {"enable", NULL};
    Connection_enableloadextension_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&:" Connection_enableloadextension_USAGE, kwlist, argcheck_bool, &enable))
      return NULL;
  }
  /* call function */
  APSW_FAULT_INJECT(EnableLoadExtensionFail,
                    PYSQLITE_CON_CALL(res = sqlite3_enable_load_extension(self->db, enable)),
                    res = SQLITE_IOERR);
  SET_EXC(res, self->db);

  /* done */
  if (res == SQLITE_OK)
    Py_RETURN_NONE;
  return NULL;
}

/** .. method:: loadextension(filename: str, entrypoint: Optional[str] = None) -> None

  Loads *filename* as an `extension <https://sqlite.org/cvstrac/wiki/wiki?p=LoadableExtensions>`_

  :param filename: The file to load.  This must be Unicode or Unicode compatible

  :param entrypoint: The initialization method to call.  If this
    parameter is not supplied then the SQLite default of
    ``sqlite3_extension_init`` is used.

  :raises ExtensionLoadingError: If the extension could not be
    loaded.  The exception string includes more details.

  -* sqlite3_load_extension

  .. seealso::

    * :meth:`~Connection.enableloadextension`
*/
static PyObject *
Connection_loadextension(Connection *self, PyObject *args, PyObject *kwds)
{
  int res;
  const char *filename = NULL, *entrypoint = NULL;
  char *errmsg = NULL; /* sqlite doesn't believe in const */

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);
  {
    static char *kwlist[] = {"filename", "entrypoint", NULL};
    Connection_loadextension_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|z:" Connection_loadextension_USAGE, kwlist, &filename, &entrypoint))
      return NULL;
  }
  PYSQLITE_CON_CALL(res = sqlite3_load_extension(self->db, filename, entrypoint, &errmsg));

  /* load_extension doesn't set the error message on the db so we have to make exception manually */
  if (res != SQLITE_OK)
  {
    assert(errmsg);
    PyErr_Format(ExcExtensionLoading, "ExtensionLoadingError: %s", errmsg ? errmsg : "unspecified");
    sqlite3_free(errmsg);
    return NULL;
  }
  Py_RETURN_NONE;
}

/* USER DEFINED FUNCTION CODE.*/
static PyTypeObject FunctionCBInfoType =
    {
        PyVarObject_HEAD_INIT(NULL, 0) "apsw.FunctionCBInfo",                   /*tp_name*/
        sizeof(FunctionCBInfo),                                                 /*tp_basicsize*/
        0,                                                                      /*tp_itemsize*/
        (destructor)FunctionCBInfo_dealloc,                                     /*tp_dealloc*/
        0,                                                                      /*tp_print*/
        0,                                                                      /*tp_getattr*/
        0,                                                                      /*tp_setattr*/
        0,                                                                      /*tp_compare*/
        0,                                                                      /*tp_repr*/
        0,                                                                      /*tp_as_number*/
        0,                                                                      /*tp_as_sequence*/
        0,                                                                      /*tp_as_mapping*/
        0,                                                                      /*tp_hash */
        0,                                                                      /*tp_call*/
        0,                                                                      /*tp_str*/
        0,                                                                      /*tp_getattro*/
        0,                                                                      /*tp_setattro*/
        0,                                                                      /*tp_as_buffer*/
        Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_VERSION_TAG, /*tp_flags*/
        "FunctionCBInfo object",                                                /* tp_doc */
        0,                                                                      /* tp_traverse */
        0,                                                                      /* tp_clear */
        0,                                                                      /* tp_richcompare */
        0,                                                                      /* tp_weaklistoffset */
        0,                                                                      /* tp_iter */
        0,                                                                      /* tp_iternext */
        0,                                                                      /* tp_methods */
        0,                                                                      /* tp_members */
        0,                                                                      /* tp_getset */
        0,                                                                      /* tp_base */
        0,                                                                      /* tp_dict */
        0,                                                                      /* tp_descr_get */
        0,                                                                      /* tp_descr_set */
        0,                                                                      /* tp_dictoffset */
        0,                                                                      /* tp_init */
        0,                                                                      /* tp_alloc */
        0,                                                                      /* tp_new */
        0,                                                                      /* tp_free */
        0,                                                                      /* tp_is_gc */
        0,                                                                      /* tp_bases */
        0,                                                                      /* tp_mro */
        0,                                                                      /* tp_cache */
        0,                                                                      /* tp_subclasses */
        0,                                                                      /* tp_weaklist */
        0,                                                                      /* tp_del */
        PyType_TRAILER};

static FunctionCBInfo *
allocfunccbinfo(void)
{
  FunctionCBInfo *res = PyObject_New(FunctionCBInfo, &FunctionCBInfoType);
  if (res)
  {
    res->name = 0;
    res->scalarfunc = 0;
    res->aggregatefactory = 0;
  }
  return res;
}

/* converts a python object into a sqlite3_context result */
static void
set_context_result(sqlite3_context *context, PyObject *obj)
{
  if (!obj)
  {
    assert(PyErr_Occurred());
    sqlite3_result_error_code(context, MakeSqliteMsgFromPyException(NULL));
    sqlite3_result_error(context, "bad object given to set_context_result", -1);
    return;
  }

  /* DUPLICATE(ish) code: this is substantially similar to the code in
     APSWCursor_dobinding.  If you fix anything here then do it there as
     well. */

  if (obj == Py_None)
  {
    sqlite3_result_null(context);
    return;
  }
  if (PyLong_Check(obj))
  {
    sqlite3_result_int64(context, PyLong_AsLongLong(obj));
    return;
  }
  if (PyFloat_Check(obj))
  {
    sqlite3_result_double(context, PyFloat_AS_DOUBLE(obj));
    return;
  }
  if (PyUnicode_Check(obj))
  {
    const char *strdata;
    Py_ssize_t strbytes;

    APSW_FAULT_INJECT(SetContextResultUnicodeConversionFails, strdata = PyUnicode_AsUTF8AndSize(obj, &strbytes), strdata = (const char *)PyErr_NoMemory());
    if (strdata)
    {
      if (strbytes > APSW_INT32_MAX)
      {
        SET_EXC(SQLITE_TOOBIG, NULL);
        sqlite3_result_error_toobig(context);
      }
      else
        sqlite3_result_text(context, strdata, strbytes, SQLITE_TRANSIENT);
    }
    else
      sqlite3_result_error(context, "Unicode conversions failed", -1);
    return;
  }

  if (PyObject_CheckBuffer(obj))
  {
    int asrb;
    Py_buffer py3buffer;

    APSW_FAULT_INJECT(SetContextResultAsReadBufferFail, asrb = PyObject_GetBuffer(obj, &py3buffer, PyBUF_SIMPLE), (PyErr_NoMemory(), asrb = -1));

    if (asrb != 0)
    {
      sqlite3_result_error(context, "PyObject_GetBuffer failed", -1);
      return;
    }
    if (py3buffer.len > APSW_INT32_MAX)
      sqlite3_result_error_toobig(context);
    else
      sqlite3_result_blob(context, py3buffer.buf, py3buffer.len, SQLITE_TRANSIENT);
    PyBuffer_Release(&py3buffer);
    return;
  }

  PyErr_Format(PyExc_TypeError, "Bad return type from function callback");
  sqlite3_result_error(context, "Bad return type from function callback", -1);
}

/* Returns a new reference to a tuple formed from function parameters */
static PyObject *
getfunctionargs(sqlite3_context *context, PyObject *firstelement, int argc, sqlite3_value **argv)
{
  PyObject *pyargs = NULL;
  int i;
  int extra = 0;

  /* extra first item */
  if (firstelement)
    extra = 1;

  APSW_FAULT_INJECT(GFAPyTuple_NewFail, pyargs = PyTuple_New((long)argc + extra), pyargs = PyErr_NoMemory());
  if (!pyargs)
  {
    sqlite3_result_error(context, "PyTuple_New failed", -1);
    goto error;
  }

  if (extra)
  {
    Py_INCREF(firstelement);
    PyTuple_SET_ITEM(pyargs, 0, firstelement);
  }

  for (i = 0; i < argc; i++)
  {
    PyObject *item = convert_value_to_pyobject(argv[i]);
    if (!item)
    {
      sqlite3_result_error(context, "convert_value_to_pyobject failed", -1);
      goto error;
    }
    PyTuple_SET_ITEM(pyargs, i + extra, item);
  }

  return pyargs;

error:
  Py_XDECREF(pyargs);
  return NULL;
}

/* dispatches scalar function */
static void
cbdispatch_func(sqlite3_context *context, int argc, sqlite3_value **argv)
{
  PyGILState_STATE gilstate;
  PyObject *pyargs = NULL;
  PyObject *retval = NULL;
  FunctionCBInfo *cbinfo = (FunctionCBInfo *)sqlite3_user_data(context);
  assert(cbinfo);

  gilstate = PyGILState_Ensure();

  assert(cbinfo->scalarfunc);

  APSW_FAULT_INJECT(CBDispatchExistingError, , PyErr_NoMemory());

  if (PyErr_Occurred())
  {
    sqlite3_result_error_code(context, MakeSqliteMsgFromPyException(NULL));
    sqlite3_result_error(context, "Prior Python Error", -1);
    goto finalfinally;
  }

  pyargs = getfunctionargs(context, NULL, argc, argv);
  if (!pyargs)
    goto finally;

  assert(!PyErr_Occurred());
  retval = PyObject_CallObject(cbinfo->scalarfunc, pyargs);
  if (retval)
    set_context_result(context, retval);

finally:
  if (PyErr_Occurred())
  {
    char *errmsg = NULL;
    char *funname = sqlite3_mprintf("user-defined-scalar-%s", cbinfo->name);
    sqlite3_result_error_code(context, MakeSqliteMsgFromPyException(&errmsg));
    sqlite3_result_error(context, errmsg, -1);
    AddTraceBackHere(__FILE__, __LINE__, funname, "{s: i, s: s}", "NumberOfArguments", argc, "message", errmsg);
    sqlite3_free(funname);
    sqlite3_free(errmsg);
  }
finalfinally:
  Py_XDECREF(pyargs);
  Py_XDECREF(retval);

  PyGILState_Release(gilstate);
}

static aggregatefunctioncontext *
getaggregatefunctioncontext(sqlite3_context *context)
{
  aggregatefunctioncontext *aggfc = sqlite3_aggregate_context(context, sizeof(aggregatefunctioncontext));
  FunctionCBInfo *cbinfo;
  PyObject *retval;
  /* have we seen it before? */
  if (aggfc->aggvalue)
    return aggfc;

  /* fill in with Py_None so we know it is valid */
  aggfc->aggvalue = Py_None;
  Py_INCREF(Py_None);

  cbinfo = (FunctionCBInfo *)sqlite3_user_data(context);
  assert(cbinfo);
  assert(cbinfo->aggregatefactory);

  /* call the aggregatefactory to get our working objects */
  retval = PyObject_CallObject(cbinfo->aggregatefactory, NULL);

  if (!retval)
    return aggfc;
  /* it should have returned a tuple of 3 items: object, stepfunction and finalfunction */
  if (!PyTuple_Check(retval))
  {
    PyErr_Format(PyExc_TypeError, "Aggregate factory should return tuple of (object, stepfunction, finalfunction)");
    goto finally;
  }
  if (PyTuple_GET_SIZE(retval) != 3)
  {
    PyErr_Format(PyExc_TypeError, "Aggregate factory should return 3 item tuple of (object, stepfunction, finalfunction)");
    goto finally;
  }
  /* we don't care about the type of the zeroth item (object) ... */

  /* stepfunc */
  if (!PyCallable_Check(PyTuple_GET_ITEM(retval, 1)))
  {
    PyErr_Format(PyExc_TypeError, "stepfunction must be callable");
    goto finally;
  }

  /* finalfunc */
  if (!PyCallable_Check(PyTuple_GET_ITEM(retval, 2)))
  {
    PyErr_Format(PyExc_TypeError, "final function must be callable");
    goto finally;
  }

  aggfc->aggvalue = PyTuple_GET_ITEM(retval, 0);
  aggfc->stepfunc = PyTuple_GET_ITEM(retval, 1);
  aggfc->finalfunc = PyTuple_GET_ITEM(retval, 2);

  Py_INCREF(aggfc->aggvalue);
  Py_INCREF(aggfc->stepfunc);
  Py_INCREF(aggfc->finalfunc);

  Py_DECREF(Py_None); /* we used this earlier as a sentinel */

finally:
  assert(retval);
  Py_DECREF(retval);
  return aggfc;
}

/*
  Note that we can't call sqlite3_result_error in the step function as
  SQLite doesn't want to you to do that (and core dumps!)
  Consequently if an error is returned, we will still be repeatedly
  called.
*/

static void
cbdispatch_step(sqlite3_context *context, int argc, sqlite3_value **argv)
{
  PyGILState_STATE gilstate;
  PyObject *pyargs;
  PyObject *retval;
  aggregatefunctioncontext *aggfc = NULL;

  gilstate = PyGILState_Ensure();

  if (PyErr_Occurred())
    goto finalfinally;

  aggfc = getaggregatefunctioncontext(context);

  if (PyErr_Occurred())
    goto finally;

  assert(aggfc);

  pyargs = getfunctionargs(context, aggfc->aggvalue, argc, argv);
  if (!pyargs)
    goto finally;

  assert(!PyErr_Occurred());
  retval = PyObject_CallObject(aggfc->stepfunc, pyargs);
  Py_DECREF(pyargs);
  Py_XDECREF(retval);

  if (!retval)
  {
    assert(PyErr_Occurred());
  }

finally:
  if (PyErr_Occurred())
  {
    char *funname = 0;
    FunctionCBInfo *cbinfo = (FunctionCBInfo *)sqlite3_user_data(context);
    assert(cbinfo);
    funname = sqlite3_mprintf("user-defined-aggregate-step-%s", cbinfo->name);
    AddTraceBackHere(__FILE__, __LINE__, funname, "{s: i}", "NumberOfArguments", argc);
    sqlite3_free(funname);
  }
finalfinally:
  PyGILState_Release(gilstate);
}

/* this is somewhat similar to cbdispatch_step, except we also have to
   do some cleanup of the aggregatefunctioncontext */
static void
cbdispatch_final(sqlite3_context *context)
{
  PyGILState_STATE gilstate;
  PyObject *retval = NULL;
  aggregatefunctioncontext *aggfc = NULL;
  PyObject *err_type = NULL, *err_value = NULL, *err_traceback = NULL;

  gilstate = PyGILState_Ensure();

  PyErr_Fetch(&err_type, &err_value, &err_traceback);

  aggfc = getaggregatefunctioncontext(context);
  assert(aggfc);

  APSW_FAULT_INJECT(CBDispatchFinalError, , PyErr_NoMemory());

  if ((err_type || err_value || err_traceback) || PyErr_Occurred() || !aggfc->finalfunc)
  {
    sqlite3_result_error(context, "Prior Python Error in step function", -1);
    goto finally;
  }

  retval = PyObject_CallFunctionObjArgs(aggfc->finalfunc, aggfc->aggvalue, NULL);
  set_context_result(context, retval);
  Py_XDECREF(retval);

finally:
  /* we also free the aggregatefunctioncontext here */
  assert(aggfc->aggvalue); /* should always be set, perhaps to Py_None */
  Py_XDECREF(aggfc->aggvalue);
  Py_XDECREF(aggfc->stepfunc);
  Py_XDECREF(aggfc->finalfunc);

  if (PyErr_Occurred() && (err_type || err_value || err_traceback))
  {
    PyErr_Format(PyExc_Exception, "An exception happened during cleanup of an aggregate function, but there was already error in the step function so only that can be returned");
    apsw_write_unraiseable(NULL);
  }

  if (err_type || err_value || err_traceback)
    PyErr_Restore(err_type, err_value, err_traceback);

  if (PyErr_Occurred())
  {
    char *funname = 0;
    FunctionCBInfo *cbinfo = (FunctionCBInfo *)sqlite3_user_data(context);
    assert(cbinfo);
    funname = sqlite3_mprintf("user-defined-aggregate-final-%s", cbinfo->name);
    AddTraceBackHere(__FILE__, __LINE__, funname, NULL);
    sqlite3_free(funname);
  }

  /* sqlite3 frees the actual underlying memory we used (aggfc itself) */

  PyGILState_Release(gilstate);
}

/* Used for the create function v2 xDestroy callbacks.  Note this is
   called even when supplying NULL for the function implementation (ie
   deleting it), so XDECREF has to be used.
 */
static void
apsw_free_func(void *funcinfo)
{
  PyGILState_STATE gilstate;
  gilstate = PyGILState_Ensure();

  Py_XDECREF((PyObject *)funcinfo);

  PyGILState_Release(gilstate);
}

/** .. method:: createscalarfunction(name: str, callable: Optional[ScalarProtocol], numargs: int = -1, deterministic: bool = False) -> None

  Registers a scalar function.  Scalar functions operate on one set of parameters once.

  :param name: The string name of the function.  It should be less than 255 characters
  :param callable: The function that will be called.  Use None to unregister.
  :param numargs: How many arguments the function takes, with -1 meaning any number
  :param deterministic: When True this means the function always
           returns the same result for the same input arguments.
           SQLite's query planner can perform additional optimisations
           for deterministic functions.  For example a random()
           function is not deterministic while one that returns the
           length of a string is.

  .. note::

    You can register the same named function but with different
    *callable* and *numargs*.  For example::

      connection.createscalarfunction("toip", ipv4convert, 4)
      connection.createscalarfunction("toip", ipv6convert, 16)
      connection.createscalarfunction("toip", strconvert, -1)

    The one with the correct *numargs* will be called and only if that
    doesn't exist then the one with negative *numargs* will be called.

  .. seealso::

     * :ref:`Example <scalar-example>`
     * :meth:`~Connection.createaggregatefunction`

  -* sqlite3_create_function_v2
*/

static PyObject *
Connection_createscalarfunction(Connection *self, PyObject *args, PyObject *kwds)
{
  int numargs = -1;
  PyObject *callable = NULL;
  int deterministic = 0;
  const char *name = 0;
  FunctionCBInfo *cbinfo;
  int res;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  {
    static char *kwlist[] = {"name", "callable", "numargs", "deterministic", NULL};
    Connection_createscalarfunction_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "sO&|iO&:" Connection_createscalarfunction_USAGE, kwlist, &name, argcheck_Optional_Callable, &callable, &numargs, argcheck_bool, &deterministic))
      return NULL;
  }
  if (!callable)
  {
    cbinfo = 0;
  }
  else
  {
    cbinfo = allocfunccbinfo();
    if (!cbinfo)
      goto finally;
    cbinfo->name = apsw_strdup(name);
    cbinfo->scalarfunc = callable;
    Py_INCREF(callable);
  }

  PYSQLITE_CON_CALL(
      res = sqlite3_create_function_v2(self->db,
                                       name,
                                       numargs,
                                       SQLITE_UTF8 | (deterministic ? SQLITE_DETERMINISTIC : 0),
                                       cbinfo,
                                       cbinfo ? cbdispatch_func : NULL,
                                       NULL,
                                       NULL,
                                       apsw_free_func));
  if (res)
  {
    /* Note: On error sqlite3_create_function_v2 calls the destructor (apsw_free_func)! */
    SET_EXC(res, self->db);
    goto finally;
  }

finally:
  if (PyErr_Occurred())
    return NULL;
  Py_RETURN_NONE;
}

/** .. method:: createaggregatefunction(name: str, factory: Optional[AggregateFactory], numargs: int = -1) -> None

  Registers an aggregate function.  Aggregate functions operate on all
  the relevant rows such as counting how many there are.

  :param name: The string name of the function.  It should be less than 255 characters
  :param factory: The function that will be called.  Use None to delete the function.
  :param numargs: How many arguments the function takes, with -1 meaning any number

  When a query starts, the *factory* will be called and must return a tuple of 3 items:

    a context object
       This can be of any type

    a step function
       This function is called once for each row.  The first parameter
       will be the context object and the remaining parameters will be
       from the SQL statement.  Any value returned will be ignored.

    a final function
       This function is called at the very end with the context object
       as a parameter.  The value returned is set as the return for
       the function. The final function is always called even if an
       exception was raised by the step function. This allows you to
       ensure any resources are cleaned up.

  .. note::

    You can register the same named function but with different
    callables and *numargs*.  See
    :meth:`~Connection.createscalarfunction` for an example.

  .. seealso::

     * :ref:`Example <aggregate-example>`
     * :meth:`~Connection.createscalarfunction`

  -* sqlite3_create_function_v2
*/

static PyObject *
Connection_createaggregatefunction(Connection *self, PyObject *args, PyObject *kwds)
{
  int numargs = -1;
  PyObject *factory;
  const char *name = 0;
  FunctionCBInfo *cbinfo;
  int res;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  {
    static char *kwlist[] = {"name", "factory", "numargs", NULL};
    Connection_createaggregatefunction_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "sO&|i:" Connection_createaggregatefunction_USAGE, kwlist, &name, argcheck_Optional_Callable, &factory, &numargs))
      return NULL;
  }

  if (!factory)
    cbinfo = 0;
  else
  {
    cbinfo = allocfunccbinfo();
    if (!cbinfo)
      goto finally;

    cbinfo->name = apsw_strdup(name);
    cbinfo->aggregatefactory = factory;
    Py_INCREF(factory);
  }

  PYSQLITE_CON_CALL(
      res = sqlite3_create_function_v2(self->db,
                                       name,
                                       numargs,
                                       SQLITE_UTF8,
                                       cbinfo,
                                       NULL,
                                       cbinfo ? cbdispatch_step : NULL,
                                       cbinfo ? cbdispatch_final : NULL,
                                       apsw_free_func));

  if (res)
  {
    /* Note: On error sqlite3_create_function_v2 calls the
   destructor (apsw_free_func)! */
    SET_EXC(res, self->db);
    goto finally;
  }

finally:
  if (PyErr_Occurred())
    return NULL;
  Py_RETURN_NONE;
}

/* USER DEFINED COLLATION CODE.*/

static int
collation_cb(void *context,
             int stringonelen, const void *stringonedata,
             int stringtwolen, const void *stringtwodata)
{
  PyGILState_STATE gilstate;
  PyObject *cbinfo = (PyObject *)context;
  PyObject *pys1 = NULL, *pys2 = NULL, *retval = NULL;
  int result = 0;

  assert(cbinfo);

  gilstate = PyGILState_Ensure();

  if (PyErr_Occurred())
    goto finally; /* outstanding error */

  pys1 = PyUnicode_FromStringAndSize(stringonedata, stringonelen);
  pys2 = PyUnicode_FromStringAndSize(stringtwodata, stringtwolen);

  if (!pys1 || !pys2)
    goto finally; /* failed to allocate strings */

  retval = PyObject_CallFunction(cbinfo, "(OO)", pys1, pys2);

  if (!retval)
  {
    AddTraceBackHere(__FILE__, __LINE__, "Collation_callback", "{s: O, s: O, s: O}", "callback", OBJ(cbinfo), "stringone", OBJ(pys1), "stringtwo", OBJ(pys2));
    goto finally; /* execution failed */
  }

  if (PyLong_Check(retval))
  {
    result = PyLong_AsLong(retval);
    goto haveval;
  }

  PyErr_Format(PyExc_TypeError, "Collation callback must return a number");
  AddTraceBackHere(__FILE__, __LINE__, "collation callback", "{s: O, s: O}",
                   "stringone", OBJ(pys1), "stringtwo", OBJ(pys2));

haveval:
  if (PyErr_Occurred())
    result = 0;

finally:
  Py_XDECREF(pys1);
  Py_XDECREF(pys2);
  Py_XDECREF(retval);
  PyGILState_Release(gilstate);
  return result;
}

static void
collation_destroy(void *context)
{
  PyGILState_STATE gilstate = PyGILState_Ensure();
  Py_DECREF((PyObject *)context);
  PyGILState_Release(gilstate);
}

/** .. method:: createcollation(name: str, callback: Optional[Callable[[str, str], int]]) -> None

  You can control how SQLite sorts (termed `collation
  <http://en.wikipedia.org/wiki/Collation>`_) when giving the
  ``COLLATE`` term to a `SELECT
  <https://sqlite.org/lang_select.html>`_.  For example your
  collation could take into account locale or do numeric sorting.

  The *callback* will be called with two items.  It should return -1
  if the first is less then the second, 0 if they are equal, and 1 if
  first is greater::

     def mycollation(one, two):
         if one < two:
             return -1
         if one == two:
             return 0
         if one > two:
             return 1

  Passing None as the callback will unregister the collation.

  .. seealso::

    * :ref:`Example <collation-example>`

  -* sqlite3_create_collation_v2
*/

static PyObject *
Connection_createcollation(Connection *self, PyObject *args, PyObject *kwds)
{
  PyObject *callback = NULL;
  const char *name = 0;
  int res;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  {
    static char *kwlist[] = {"name", "callback", NULL};
    Connection_createcollation_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "sO&:" Connection_createcollation_USAGE, kwlist, &name, argcheck_Optional_Callable, &callback))
      return NULL;
  }

  PYSQLITE_CON_CALL(
      res = sqlite3_create_collation_v2(self->db,
                                        name,
                                        SQLITE_UTF8,
                                        callback ? callback : NULL,
                                        callback ? collation_cb : NULL,
                                        callback ? collation_destroy : NULL));

  if (res != SQLITE_OK)
  {
    SET_EXC(res, self->db);
    return NULL;
  }

  if (callback)
    Py_INCREF(callback);

  Py_RETURN_NONE;
}

/** .. method:: filecontrol(dbname: str, op: int, pointer: int) -> bool

  Calls the :meth:`~VFSFile.xFileControl` method on the :ref:`VFS`
  implementing :class:`file access <VFSFile>` for the database.

  :param dbname: The name of the database to affect (eg "main", "temp", attached name)
  :param op: A `numeric code
    <https://sqlite.org/c3ref/c_fcntl_lockstate.html>`_ with values less
    than 100 reserved for SQLite internal use.
  :param pointer: A number which is treated as a ``void pointer`` at the C level.

  :returns: True or False indicating if the VFS understood the op.

  If you want data returned back then the *pointer* needs to point to
  something mutable.  Here is an example using `ctypes
  <https://docs.python.org/3/library/ctypes.html>`_ of
  passing a Python dictionary to :meth:`~VFSFile.xFileControl` which
  can then modify the dictionary to set return values::

    obj={"foo": 1, 2: 3}                 # object we want to pass
    objwrap=ctypes.py_object(obj)        # objwrap must live before and after the call else
                                         # it gets garbage collected
    connection.filecontrol(
             "main",                     # which db
             123,                        # our op code
             ctypes.addressof(objwrap))  # get pointer

  The :meth:`~VFSFile.xFileControl` method then looks like this::

    def xFileControl(self, op, pointer):
        if op==123:                      # our op code
            obj=ctypes.py_object.from_address(pointer).value
            # play with obj - you can use id() to verify it is the same
            print(obj["foo"])
            obj["result"]="it worked"
            return True
        else:
            # pass to parent/superclass
            return super(MyFile, self).xFileControl(op, pointer)

  This is how you set the chunk size by which the database grows.  Do
  not combine it into one line as the c_int would be garbage collected
  before the filecontrol call is made::

     chunksize=ctypes.c_int(32768)
     connection.filecontrol("main", apsw.SQLITE_FCNTL_CHUNK_SIZE, ctypes.addressof(chunksize))

  -* sqlite3_file_control
*/

static PyObject *
Connection_filecontrol(Connection *self, PyObject *args, PyObject *kwds)
{
  void *pointer;
  int res = SQLITE_ERROR, op;
  const char *dbname = NULL;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  {
    static char *kwlist[] = {"dbname", "op", "pointer", NULL};
    Connection_filecontrol_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "siO&:" Connection_filecontrol_USAGE, kwlist, &dbname, &op, argcheck_pointer, &pointer))
      return NULL;
  }

  PYSQLITE_CON_CALL(res = sqlite3_file_control(self->db, dbname, op, pointer));

  if (res != SQLITE_OK && res != SQLITE_NOTFOUND)
    SET_EXC(res, self->db);

  if (PyErr_Occurred())
    return NULL;

  if (res == SQLITE_NOTFOUND)
    Py_RETURN_FALSE;
  Py_RETURN_TRUE;
}

/** .. method:: sqlite3pointer() -> int

  Returns the underlying `sqlite3 *
  <https://sqlite.org/c3ref/sqlite3.html>`_ for the connection. This
  method is useful if there are other C level libraries in the same
  process and you want them to use the APSW connection handle. The
  value is returned as a number using :meth:`PyLong_FromVoidPtr` under the
  hood. You should also ensure that you increment the reference count on
  the :class:`Connection` for as long as the other libraries are using
  the pointer.  It is also a very good idea to call
  :meth:`sqlitelibversion` and ensure it is the same as the other
  libraries.

*/
static PyObject *
Connection_sqlite3pointer(Connection *self)
{
  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  return PyLong_FromVoidPtr(self->db);
}

/** .. method:: wal_autocheckpoint(n: int) -> None

    Sets how often the :ref:`wal` checkpointing is run.

    :param n: A number representing the checkpointing interval or
      zero/negative to disable auto checkpointing.

   -* sqlite3_wal_autocheckpoint
*/
static PyObject *
Connection_wal_autocheckpoint(Connection *self, PyObject *args, PyObject *kwds)
{
  int n, res;
  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  {
    static char *kwlist[] = {"n", NULL};
    Connection_wal_autocheckpoint_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "i:" Connection_wal_autocheckpoint_USAGE, kwlist, &n))
      return NULL;
  }

  APSW_FAULT_INJECT(WalAutocheckpointFails,
                    PYSQLITE_CON_CALL(res = sqlite3_wal_autocheckpoint(self->db, n)),
                    res = SQLITE_IOERR);

  SET_EXC(res, self->db);

  /* done */
  if (res == SQLITE_OK)
    Py_RETURN_NONE;
  return NULL;
}

/** .. method:: wal_checkpoint(dbname: Optional[str] = None, mode: int = apsw.SQLITE_CHECKPOINT_PASSIVE) -> Tuple[int, int]

    Does a WAL checkpoint.  Has no effect if the database(s) are not in WAL mode.

    :param dbname:  The name of the database or all databases if None

    :param mode: One of the `checkpoint modes <https://sqlite.org/c3ref/wal_checkpoint_v2.html>`__.

    :return: A tuple of the size of the WAL log in frames and the
       number of frames checkpointed as described in the
       `documentation
       <https://sqlite.org/c3ref/wal_checkpoint_v2.html>`__.

  -* sqlite3_wal_checkpoint_v2
*/
static PyObject *
Connection_wal_checkpoint(Connection *self, PyObject *args, PyObject *kwds)
{
  int res;
  const char *dbname = NULL;
  int mode = SQLITE_CHECKPOINT_PASSIVE;
  int nLog = 0, nCkpt = 0;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  {
    static char *kwlist[] = {"dbname", "mode", NULL};
    Connection_wal_checkpoint_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|zi:" Connection_wal_checkpoint_USAGE, kwlist, &dbname, &mode))
      return NULL;
  }
  APSW_FAULT_INJECT(WalCheckpointFails,
                    PYSQLITE_CON_CALL(res = sqlite3_wal_checkpoint_v2(self->db, dbname, mode, &nLog, &nCkpt)),
                    res = SQLITE_IOERR);

  SET_EXC(res, self->db);

  /* done */
  if (res == SQLITE_OK)
    return Py_BuildValue("ii", nLog, nCkpt);
  return NULL;
}

static struct sqlite3_module apsw_vtable_module;
static void apswvtabFree(void *context);

/** .. method:: createmodule(name: str, datasource: Any) -> None

    Registers a virtual table.  See :ref:`virtualtables` for details.

    .. seealso::

       * :ref:`Example <example-vtable>`

    -* sqlite3_create_module_v2
*/
static PyObject *
Connection_createmodule(Connection *self, PyObject *args, PyObject *kwds)
{
  const char *name = NULL;
  PyObject *datasource = NULL;
  vtableinfo *vti;
  int res;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  {
    static char *kwlist[] = {"name", "datasource", NULL};
    Connection_createmodule_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "sO:" Connection_createmodule_USAGE, kwlist, &name, &datasource))
      return NULL;
  }
  Py_INCREF(datasource);
  vti = PyMem_Malloc(sizeof(vtableinfo));
  vti->connection = self;
  vti->datasource = datasource;

  /* SQLite is really finnicky.  Note that it calls the destructor on
     failure  */
  APSW_FAULT_INJECT(CreateModuleFail,
                    PYSQLITE_CON_CALL((res = sqlite3_create_module_v2(self->db, name, &apsw_vtable_module, vti, apswvtabFree), vti = NULL)),
                    res = SQLITE_IOERR);
  SET_EXC(res, self->db);

  if (res != SQLITE_OK)
  {
    if (vti)
      apswvtabFree(vti);
    return NULL;
  }

  Py_RETURN_NONE;
}

/** .. method:: overloadfunction(name: str, nargs: int) -> None

  Registers a placeholder function so that a virtual table can provide an implementation via
  :meth:`VTTable.FindFunction`.

  :param name: Function name
  :param nargs: How many arguments the function takes

  Due to :cvstrac:`3507` underlying errors will not be returned.

  -* sqlite3_overload_function
*/
static PyObject *
Connection_overloadfunction(Connection *self, PyObject *args, PyObject *kwds)
{
  const char *name;
  int nargs, res;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);
  {
    static char *kwlist[] = {"name", "nargs", NULL};
    Connection_overloadfunction_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "si:" Connection_overloadfunction_USAGE, kwlist, &name, &nargs))
      return NULL;
  }
  APSW_FAULT_INJECT(OverloadFails,
                    PYSQLITE_CON_CALL(res = sqlite3_overload_function(self->db, name, nargs)),
                    res = SQLITE_NOMEM);

  SET_EXC(res, self->db);

  if (res)
    return NULL;

  Py_RETURN_NONE;
}

/** .. method:: setexectrace(callable: Optional[ExecTracer]) -> None

  *callable* is called with the cursor, statement and bindings for
  each :meth:`~Cursor.execute` or :meth:`~Cursor.executemany` on this
  Connection, unless the :class:`Cursor` installed its own
  tracer. Your execution tracer can also abort execution of a
  statement.

  If *callable* is :const:`None` then any existing execution tracer is
  removed.

  .. seealso::

    * :ref:`tracing`
    * :ref:`rowtracer`
    * :meth:`Cursor.setexectrace`
*/

static PyObject *
Connection_setexectrace(Connection *self, PyObject *args, PyObject *kwds)
{
  PyObject *callable;
  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  {
    static char *kwlist[] = {"callable", NULL};
    Connection_setexectrace_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&:" Connection_setexectrace_USAGE, kwlist, argcheck_Optional_Callable, &callable))
      return NULL;
  }

  Py_XINCREF(callable);
  Py_XDECREF(self->exectrace);
  self->exectrace = callable;

  Py_RETURN_NONE;
}

/** .. method:: setrowtrace(callable: Optional[RowTracer]) -> None

  *callable* is called with the cursor and row being returned for
  :class:`cursors <Cursor>` associated with this Connection, unless
  the Cursor installed its own tracer.  You can change the data that
  is returned or cause the row to be skipped altogether.

  If *callable* is :const:`None` then any existing row tracer is
  unregistered.

  .. seealso::

    * :ref:`tracing`
    * :ref:`rowtracer`
    * :meth:`Cursor.setexectrace`
*/

static PyObject *
Connection_setrowtrace(Connection *self, PyObject *args, PyObject *kwds)
{
  PyObject *callable;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  {
    static char *kwlist[] = {"callable", NULL};
    Connection_setrowtrace_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&:" Connection_setrowtrace_USAGE, kwlist, argcheck_Optional_Callable, &callable))
      return NULL;
  }

  Py_XINCREF(callable);
  Py_XDECREF(self->rowtrace);
  self->rowtrace = callable;

  Py_RETURN_NONE;
}

/** .. method:: getexectrace() -> Optional[ExecTracer]

  Returns the currently installed (via :meth:`~Connection.setexectrace`)
  execution tracer.

  .. seealso::

    * :ref:`tracing`
*/
static PyObject *
Connection_getexectrace(Connection *self)
{
  PyObject *ret;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  ret = (self->exectrace) ? (self->exectrace) : Py_None;
  Py_INCREF(ret);
  return ret;
}

/** .. method:: getrowtrace() -> Optional[RowTracer]

  Returns the currently installed (via :meth:`~Connection.setrowtrace`)
  row tracer.

  .. seealso::

    * :ref:`tracing`
*/
static PyObject *
Connection_getrowtrace(Connection *self)
{
  PyObject *ret;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  ret = (self->rowtrace) ? (self->rowtrace) : Py_None;
  Py_INCREF(ret);
  return ret;
}

/** .. method:: __enter__() -> Connection

  You can use the database as a `context manager
  <http://docs.python.org/reference/datamodel.html#with-statement-context-managers>`_
  as defined in :pep:`0343`.  When you use *with* a transaction is
  started.  If the block finishes with an exception then the
  transaction is rolled back, otherwise it is committed.  For example::

    with connection:
        connection.cursor().execute("....")
        with connection:
            # nested is supported
            call_function(connection)
            connection.cursor().execute("...")
            with connection as db:
                # You can also use 'as'
                call_function2(db)
                db.cursor().execute("...")

  Behind the scenes the `savepoint
  <https://sqlite.org/lang_savepoint.html>`_ functionality introduced in
  SQLite 3.6.8 is used to provide nested transactions.
*/
static PyObject *
Connection_enter(Connection *self)
{
  char *sql = 0;
  int res;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  sql = sqlite3_mprintf("SAVEPOINT \"_apsw-%ld\"", self->savepointlevel);
  if (!sql)
    return PyErr_NoMemory();

  /* exec tracing - we allow it to prevent */
  if (self->exectrace && self->exectrace != Py_None)
  {
    int result;
    PyObject *retval = PyObject_CallFunction(self->exectrace, "OsO", self, sql, Py_None);
    if (!retval)
      goto error;
    result = PyObject_IsTrue(retval);
    Py_DECREF(retval);
    if (result == -1)
    {
      assert(PyErr_Occurred());
      goto error;
    }
    if (result == 0)
    {
      PyErr_Format(ExcTraceAbort, "Aborted by false/null return value of exec tracer");
      goto error;
    }
    assert(result == 1);
  }

  APSW_FAULT_INJECT(ConnectionEnterExecFailed,
                    PYSQLITE_CON_CALL(res = sqlite3_exec(self->db, sql, 0, 0, 0)),
                    res = SQLITE_NOMEM);
  sqlite3_free(sql);
  SET_EXC(res, self->db);
  if (res)
    return NULL;

  self->savepointlevel++;
  Py_INCREF(self);
  return (PyObject *)self;

error:
  assert(PyErr_Occurred());
  if (sql)
    sqlite3_free(sql);
  return NULL;
}

/** .. method:: __exit__() -> Literal[False]

  Implements context manager in conjunction with
  :meth:`~Connection.__enter__`.  Any exception that happened in the
  *with* block is raised after committing or rolling back the
  savepoint.
*/

/* A helper function.  Returns -1 on memory error, 0 on failure and 1 on success */
static int connection_trace_and_exec(Connection *self, int release, int sp, int continue_on_trace_error)
{
  char *sql;
  int res;

  sql = sqlite3_mprintf(release ? "RELEASE SAVEPOINT \"_apsw-%ld\"" : "ROLLBACK TO SAVEPOINT \"_apsw-%ld\"",
                        sp);
  if (!sql)
  {
    PyErr_NoMemory();
    return -1;
  }

  if (self->exectrace && self->exectrace != Py_None)
  {
    PyObject *result;
    PyObject *etype = NULL, *eval = NULL, *etb = NULL;

    if (PyErr_Occurred())
      PyErr_Fetch(&etype, &eval, &etb);

    result = PyObject_CallFunction(self->exectrace, "OsO", self, sql, Py_None);
    Py_XDECREF(result);

    if (etype || eval || etb)
      PyErr_Restore(etype, eval, etb);

    if (!result && !continue_on_trace_error)
    {
      sqlite3_free(sql);
      return 0;
    }
  }

  PYSQLITE_CON_CALL(res = sqlite3_exec(self->db, sql, 0, 0, 0));
  SET_EXC(res, self->db);
  sqlite3_free(sql);
  assert(res == SQLITE_OK || PyErr_Occurred());
  return res == SQLITE_OK;
}

static PyObject *
Connection_exit(Connection *self, PyObject *args)
{
  PyObject *etype, *evalue, *etb;
  long sp;
  int res;
  int return_null = 0;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  /* the builtin python __exit__ implementations don't error if you
     call __exit__ without corresponding enters */
  if (self->savepointlevel == 0)
    Py_RETURN_FALSE;

  /* We always pop a level, irrespective of how this function returns
     - (ie successful or error) */
  if (self->savepointlevel)
    self->savepointlevel--;
  sp = self->savepointlevel;

  if (!PyArg_ParseTuple(args, "OOO", &etype, &evalue, &etb))
    return NULL;

  /* try the commit first because it may fail in which case we'll need
     to roll it back - see issue 98 */
  if (etype == Py_None && evalue == Py_None && etb == Py_None)
  {
    res = connection_trace_and_exec(self, 1, sp, 0);
    if (res == -1)
      return NULL;
    if (res == 1)
      Py_RETURN_FALSE;
    assert(res == 0);
    assert(PyErr_Occurred());
    return_null = 1;
  }

  res = connection_trace_and_exec(self, 0, sp, 1);
  if (res == -1)
    return NULL;
  return_null = return_null || res == 0;
  /* we have rolled back, but still need to release the savepoint */
  res = connection_trace_and_exec(self, 1, sp, 1);
  return_null = return_null || res == 0;

  if (return_null)
    return NULL;
  Py_RETURN_FALSE;
}

/** .. method:: config(op: int, *args: int) -> int

    :param op: A `configuration operation
      <https://sqlite.org/c3ref/c_dbconfig_enable_fkey.html>`__
    :param args: Zero or more arguments as appropriate for *op*

    Only optiona that take an int and return one are implemented.

    -* sqlite3_db_config
*/
static PyObject *
Connection_config(Connection *self, PyObject *args)
{
  long opt;
  int res;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  if (PyTuple_GET_SIZE(args) < 1 || !PyLong_Check(PyTuple_GET_ITEM(args, 0)))
    return PyErr_Format(PyExc_TypeError, "There should be at least one argument with the first being a number");

  opt = PyLong_AsLong(PyTuple_GET_ITEM(args, 0));
  if (PyErr_Occurred())
    return NULL;

  switch (opt)
  {
  case SQLITE_DBCONFIG_ENABLE_FKEY:
  case SQLITE_DBCONFIG_ENABLE_TRIGGER:
  case SQLITE_DBCONFIG_ENABLE_FTS3_TOKENIZER:
  case SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION:
  case SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE:
  case SQLITE_DBCONFIG_ENABLE_QPSG:
  case SQLITE_DBCONFIG_RESET_DATABASE:
  case SQLITE_DBCONFIG_DEFENSIVE:
  case SQLITE_DBCONFIG_WRITABLE_SCHEMA:
  case SQLITE_DBCONFIG_LEGACY_ALTER_TABLE:
  case SQLITE_DBCONFIG_DQS_DML:
  case SQLITE_DBCONFIG_DQS_DDL:
  case SQLITE_DBCONFIG_ENABLE_VIEW:
  {
    int opdup, val, current;
    if (!PyArg_ParseTuple(args, "ii", &opdup, &val))
      return NULL;

    APSW_FAULT_INJECT(DBConfigFails,
                      PYSQLITE_CON_CALL(res = sqlite3_db_config(self->db, opdup, val, &current)),
                      res = SQLITE_NOMEM);
    if (res != SQLITE_OK)
    {
      SET_EXC(res, self->db);
      return NULL;
    }
    return PyLong_FromLong(current);
  }
  default:
    return PyErr_Format(PyExc_ValueError, "Unknown config operation %d", (int)opt);
  }
}

/** .. method:: status(op: int, reset: bool = False) -> Tuple[int, int]

  Returns current and highwater measurements for the database.

  :param op: A `status parameter <https://sqlite.org/c3ref/c_dbstatus_options.html>`_
  :param reset: If *True* then the highwater is set to the current value
  :returns: A tuple of current value and highwater value

  .. seealso::

    The :func:`status` example which works in exactly the same way.

    * :ref:`Status example <example-status>`

  -* sqlite3_db_status

*/
static PyObject *
Connection_status(Connection *self, PyObject *args, PyObject *kwds)
{
  int res, op, current = 0, highwater = 0, reset = 0;
  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  {
    static char *kwlist[] = {"op", "reset", NULL};
    Connection_status_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "i|O&:" Connection_status_USAGE, kwlist, &op, argcheck_bool, &reset))
      return NULL;
  }

  PYSQLITE_CON_CALL(res = sqlite3_db_status(self->db, op, &current, &highwater, reset));
  SET_EXC(res, NULL);

  if (res != SQLITE_OK)
    return NULL;

  return Py_BuildValue("(ii)", current, highwater);
}

/** .. method:: readonly(name: str) -> bool

  True or False if the named (attached) database was opened readonly or file
  permissions don't allow writing.  The main database is named "main".

  An exception is raised if the database doesn't exist.

  -* sqlite3_db_readonly

*/
static PyObject *
Connection_readonly(Connection *self, PyObject *args, PyObject *kwds)
{
  int res = -1;
  const char *name;

  CHECK_CLOSED(self, NULL);
  {
    static char *kwlist[] = {"name", NULL};
    Connection_readonly_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s:" Connection_readonly_USAGE, kwlist, &name))
      return NULL;
  }
  res = sqlite3_db_readonly(self->db, name);

  if (res == 1)
    Py_RETURN_TRUE;
  if (res == 0)
    Py_RETURN_FALSE;

  return PyErr_Format(exc_descriptors[0].cls, "Unknown database name");
}

/** .. method:: db_filename(name: str) -> str

  Returns the full filename of the named (attached) database.  The
  main database is named "main".

  -* sqlite3_db_filename
*/
static PyObject *
Connection_db_filename(Connection *self, PyObject *args, PyObject *kwds)
{
  const char *res;
  const char *name;
  CHECK_CLOSED(self, NULL);

  {
    static char *kwlist[] = {"name", NULL};
    Connection_db_filename_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s:" Connection_db_filename_USAGE, kwlist, &name))
      return NULL;
  }

  res = sqlite3_db_filename(self->db, name);

  return convertutf8string(res);
}

/** .. method:: txn_state(schema: Optional[str] = None) -> int

  Returns the current transaction state of the database, or a specific schema
  if provided.  ValueError is raised if schema is not None or a valid schema name.
  :attr:`apsw.mapping_txn_state` contains the names and values returned.

  -* sqlite3_txn_state
*/

static PyObject *
Connection_txn_state(Connection *self, PyObject *args, PyObject *kwds)
{
  const char *schema = NULL;
  int res;
  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  {
    static char *kwlist[] = {"schema", NULL};
    Connection_txn_state_CHECK;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|z:" Connection_txn_state_USAGE, kwlist, &schema))
      return NULL;
  }
  PYSQLITE_CON_CALL(res = sqlite3_txn_state(self->db, schema));

  if (res >= 0)
    return PyLong_FromLong(res);

  return PyErr_Format(PyExc_ValueError, "unknown schema");
}

/** .. attribute:: filename
  :type: str

  The filename of the  database.

  -* sqlite3_db_filename
*/

static PyObject *
Connection_getmainfilename(Connection *self)
{
  CHECK_CLOSED(self, NULL);
  return convertutf8string(sqlite3_db_filename(self->db, "main"));
}

static PyGetSetDef Connection_getseters[] = {
    /* name getter setter doc closure */
    {"filename",
     (getter)Connection_getmainfilename, NULL,
     Connection_filename_DOC, NULL},
    /* Sentinel */
    {NULL, NULL, NULL, NULL, NULL}};

/** .. attribute:: open_flags
  :type: int

  The integer flags used to open the database.
*/

/** .. attribute:: open_vfs
  :type: str

  The string name of the vfs used to open the database.
*/

static int
Connection_tp_traverse(Connection *self, visitproc visit, void *arg)
{
  Py_VISIT(self->busyhandler);
  Py_VISIT(self->rollbackhook);
  Py_VISIT(self->profile);
  Py_VISIT(self->updatehook);
  Py_VISIT(self->commithook);
  Py_VISIT(self->walhook);
  Py_VISIT(self->progresshandler);
  Py_VISIT(self->authorizer);
  Py_VISIT(self->collationneeded);
  Py_VISIT(self->exectrace);
  Py_VISIT(self->rowtrace);
  Py_VISIT(self->vfs);
  Py_VISIT(self->dependents);
  return 0;
}

static PyMemberDef Connection_members[] = {
    /* name type offset flags doc */
    {"open_flags", T_OBJECT, offsetof(Connection, open_flags), READONLY, Connection_open_flags_DOC},
    {"open_vfs", T_OBJECT, offsetof(Connection, open_vfs), READONLY, Connection_open_vfs_DOC},
    {0, 0, 0, 0, 0}};

static PyMethodDef Connection_methods[] = {
    {"cursor", (PyCFunction)Connection_cursor, METH_NOARGS,
     Connection_cursor_DOC},
    {"close", (PyCFunction)Connection_close, METH_VARARGS | METH_KEYWORDS,
     Connection_close_DOC},
    {"setbusytimeout", (PyCFunction)Connection_setbusytimeout, METH_VARARGS | METH_KEYWORDS,
     Connection_setbusytimeout_DOC},
    {"interrupt", (PyCFunction)Connection_interrupt, METH_NOARGS,
     Connection_interrupt_DOC},
    {"createscalarfunction", (PyCFunction)Connection_createscalarfunction, METH_VARARGS | METH_KEYWORDS,
     Connection_createscalarfunction_DOC},
    {"createaggregatefunction", (PyCFunction)Connection_createaggregatefunction, METH_VARARGS | METH_KEYWORDS,
     Connection_createaggregatefunction_DOC},
    {"setbusyhandler", (PyCFunction)Connection_setbusyhandler, METH_VARARGS | METH_KEYWORDS,
     Connection_setbusyhandler_DOC},
    {"changes", (PyCFunction)Connection_changes, METH_NOARGS,
     Connection_changes_DOC},
    {"totalchanges", (PyCFunction)Connection_totalchanges, METH_NOARGS,
     Connection_totalchanges_DOC},
    {"getautocommit", (PyCFunction)Connection_getautocommit, METH_NOARGS,
     Connection_getautocommit_DOC},
    {"createcollation", (PyCFunction)Connection_createcollation, METH_VARARGS | METH_KEYWORDS,
     Connection_createcollation_DOC},
    {"last_insert_rowid", (PyCFunction)Connection_last_insert_rowid, METH_NOARGS,
     Connection_last_insert_rowid_DOC},
    {"set_last_insert_rowid", (PyCFunction)Connection_set_last_insert_rowid, METH_VARARGS | METH_KEYWORDS,
     Connection_set_last_insert_rowid_DOC},
    {"collationneeded", (PyCFunction)Connection_collationneeded, METH_VARARGS | METH_KEYWORDS,
     Connection_collationneeded_DOC},
    {"setauthorizer", (PyCFunction)Connection_setauthorizer, METH_VARARGS | METH_KEYWORDS,
     Connection_setauthorizer_DOC},
    {"setupdatehook", (PyCFunction)Connection_setupdatehook, METH_VARARGS | METH_KEYWORDS,
     Connection_setupdatehook_DOC},
    {"setrollbackhook", (PyCFunction)Connection_setrollbackhook, METH_VARARGS | METH_KEYWORDS,
     Connection_setrollbackhook_DOC},
    {"blobopen", (PyCFunction)Connection_blobopen, METH_VARARGS | METH_KEYWORDS,
     Connection_blobopen_DOC},
    {"setprogresshandler", (PyCFunction)Connection_setprogresshandler, METH_VARARGS | METH_KEYWORDS,
     Connection_setprogresshandler_DOC},
    {"setcommithook", (PyCFunction)Connection_setcommithook, METH_VARARGS | METH_KEYWORDS,
     Connection_setcommithook_DOC},
    {"setwalhook", (PyCFunction)Connection_setwalhook, METH_VARARGS | METH_KEYWORDS,
     Connection_setwalhook_DOC},
    {"limit", (PyCFunction)Connection_limit, METH_VARARGS | METH_KEYWORDS,
     Connection_limit_DOC},
    {"setprofile", (PyCFunction)Connection_setprofile, METH_VARARGS | METH_KEYWORDS,
     Connection_setprofile_DOC},
#if !defined(SQLITE_OMIT_LOAD_EXTENSION)
    {"enableloadextension", (PyCFunction)Connection_enableloadextension, METH_VARARGS | METH_KEYWORDS,
     Connection_enableloadextension_DOC},
    {"loadextension", (PyCFunction)Connection_loadextension, METH_VARARGS | METH_KEYWORDS,
     Connection_loadextension_DOC},
#endif
    {"createmodule", (PyCFunction)Connection_createmodule, METH_VARARGS | METH_KEYWORDS,
     Connection_createmodule_DOC},
    {"overloadfunction", (PyCFunction)Connection_overloadfunction, METH_VARARGS | METH_KEYWORDS,
     Connection_overloadfunction_DOC},
    {"backup", (PyCFunction)Connection_backup, METH_VARARGS | METH_KEYWORDS,
     Connection_backup_DOC},
    {"filecontrol", (PyCFunction)Connection_filecontrol, METH_VARARGS | METH_KEYWORDS,
     Connection_filecontrol_DOC},
    {"sqlite3pointer", (PyCFunction)Connection_sqlite3pointer, METH_NOARGS,
     Connection_sqlite3pointer_DOC},
    {"setexectrace", (PyCFunction)Connection_setexectrace, METH_VARARGS | METH_KEYWORDS,
     Connection_setexectrace_DOC},
    {"setrowtrace", (PyCFunction)Connection_setrowtrace, METH_VARARGS | METH_KEYWORDS,
     Connection_setrowtrace_DOC},
    {"getexectrace", (PyCFunction)Connection_getexectrace, METH_NOARGS,
     Connection_getexectrace_DOC},
    {"getrowtrace", (PyCFunction)Connection_getrowtrace, METH_NOARGS,
     Connection_getrowtrace_DOC},
    {"__enter__", (PyCFunction)Connection_enter, METH_NOARGS,
     Connection_enter_DOC},
    {"__exit__", (PyCFunction)Connection_exit, METH_VARARGS,
     Connection_exit_DOC},
    {"wal_autocheckpoint", (PyCFunction)Connection_wal_autocheckpoint, METH_VARARGS | METH_KEYWORDS,
     Connection_wal_autocheckpoint_DOC},
    {"wal_checkpoint", (PyCFunction)Connection_wal_checkpoint, METH_VARARGS | METH_KEYWORDS,
     Connection_wal_checkpoint_DOC},
    {"config", (PyCFunction)Connection_config, METH_VARARGS,
     Connection_config_DOC},
    {"status", (PyCFunction)Connection_status, METH_VARARGS | METH_KEYWORDS,
     Connection_status_DOC},
    {"readonly", (PyCFunction)Connection_readonly, METH_VARARGS | METH_KEYWORDS,
     Connection_readonly_DOC},
    {"db_filename", (PyCFunction)Connection_db_filename, METH_VARARGS | METH_KEYWORDS,
     Connection_db_filename_DOC},
    {"txn_state", (PyCFunction)Connection_txn_state, METH_VARARGS | METH_KEYWORDS,
     Connection_txn_state_DOC},
    {"serialize", (PyCFunction)Connection_serialize, METH_VARARGS | METH_KEYWORDS,
     Connection_serialize_DOC},
    {"deserialize", (PyCFunction)Connection_deserialize, METH_VARARGS | METH_KEYWORDS,
     Connection_deserialize_DOC},
    {"autovacuum_pages", (PyCFunction)Connection_autovacuum_pages, METH_VARARGS | METH_KEYWORDS,
     Connection_autovacuum_pages_DOC},
    {"db_names", (PyCFunction)Connection_db_names, METH_NOARGS,
     Connection_db_names_DOC},
    {0, 0, 0, 0} /* Sentinel */
};

static PyTypeObject ConnectionType =
    {
        PyVarObject_HEAD_INIT(NULL, 0) "apsw.Connection",                                            /*tp_name*/
        sizeof(Connection),                                                                          /*tp_basicsize*/
        0,                                                                                           /*tp_itemsize*/
        (destructor)Connection_dealloc,                                                              /*tp_dealloc*/
        0,                                                                                           /*tp_print*/
        0,                                                                                           /*tp_getattr*/
        0,                                                                                           /*tp_setattr*/
        0,                                                                                           /*tp_compare*/
        0,                                                                                           /*tp_repr*/
        0,                                                                                           /*tp_as_number*/
        0,                                                                                           /*tp_as_sequence*/
        0,                                                                                           /*tp_as_mapping*/
        0,                                                                                           /*tp_hash */
        0,                                                                                           /*tp_call*/
        0,                                                                                           /*tp_str*/
        0,                                                                                           /*tp_getattro*/
        0,                                                                                           /*tp_setattro*/
        0,                                                                                           /*tp_as_buffer*/
        Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_VERSION_TAG | Py_TPFLAGS_HAVE_GC, /*tp_flags*/
        Connection_init_DOC,                                                                         /* tp_doc */
        (traverseproc)Connection_tp_traverse,                                                        /* tp_traverse */
        0,                                                                                           /* tp_clear */
        0,                                                                                           /* tp_richcompare */
        offsetof(Connection, weakreflist),                                                           /* tp_weaklistoffset */
        0,                                                                                           /* tp_iter */
        0,                                                                                           /* tp_iternext */
        Connection_methods,                                                                          /* tp_methods */
        Connection_members,                                                                          /* tp_members */
        Connection_getseters,                                                                        /* tp_getset */
        0,                                                                                           /* tp_base */
        0,                                                                                           /* tp_dict */
        0,                                                                                           /* tp_descr_get */
        0,                                                                                           /* tp_descr_set */
        0,                                                                                           /* tp_dictoffset */
        (initproc)Connection_init,                                                                   /* tp_init */
        0,                                                                                           /* tp_alloc */
        Connection_new,                                                                              /* tp_new */
        0,                                                                                           /* tp_free */
        0,                                                                                           /* tp_is_gc */
        0,                                                                                           /* tp_bases */
        0,                                                                                           /* tp_mro */
        0,                                                                                           /* tp_cache */
        0,                                                                                           /* tp_subclasses */
        0,                                                                                           /* tp_weaklist */
        0,                                                                                           /* tp_del */
        PyType_TRAILER};
