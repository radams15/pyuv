
static PyObject* PyExc_FSError;

typedef struct {
    Loop *loop;
    PyObject *callback;
    PyObject *data;
} fs_req_data_t;


static PyObject *
format_time(time_t sec, unsigned long nsec)
{
    PyObject *ival;
#if SIZEOF_TIME_T > SIZEOF_LONG
    ival = PyLong_FromLongLong((PY_LONG_LONG)sec);
#else
    ival = PyInt_FromLong((long)sec);
#endif
    if (!ival) {
        return PyInt_FromLong(0);
    }
    return ival;
}


static void
stat_cb(uv_fs_t* req) {
    PyGILState_STATE gstate = PyGILState_Ensure();
    ASSERT(req);
    ASSERT(req->fs_type == UV_FS_STAT || req->fs_type == UV_FS_LSTAT);

    struct stat *st = (struct stat *)(req->ptr);
    fs_req_data_t *req_data = (fs_req_data_t*)(req->data);

    unsigned long ansec, mnsec, cnsec;
    PyObject *result = PyInt_FromLong((long)req->result);
    PyObject *errorno = PyInt_FromLong((long)req->errorno);
    PyObject *stat_data = PyTuple_New(13);

    if (!(result && errorno && stat_data)) {
        PyErr_NoMemory();
        PyErr_WriteUnraisable(req_data->callback);
        goto stat_end;
    }

    if (req->result != -1) {
        PyTuple_SET_ITEM(stat_data, 0, PyInt_FromLong((long)st->st_mode));
#ifdef HAVE_LARGEFILE_SUPPORT
        PyTple_SET_ITEM(stat_data, 1, PyLong_FromLongLong((PY_LONG_LONG)st->st_ino));
#else
        PyTuple_SET_ITEM(stat_data, 1, PyInt_FromLong((long)st->st_ino));
#endif
#if defined(HAVE_LONG_LONG)
        PyTuple_SET_ITEM(stat_data, 2, PyLong_FromLongLong((PY_LONG_LONG)st->st_dev));
#else
        PyTuple_SET_ITEM(stat_data, 2, PyInt_FromLong((long)st->st_dev));
#endif
        PyTuple_SET_ITEM(stat_data, 3, PyInt_FromLong((long)st->st_nlink));
        PyTuple_SET_ITEM(stat_data, 4, PyInt_FromLong((long)st->st_uid));
        PyTuple_SET_ITEM(stat_data, 5, PyInt_FromLong((long)st->st_gid));
#ifdef HAVE_LARGEFILE_SUPPORT
        PyTuple_SET_ITEM(stat_data, 6, PyLong_FromLongLong((PY_LONG_LONG)st->st_size));
#else
        PyTuple_SET_ITEM(stat_data, 6, PyInt_FromLong(st->st_size));
#endif
#if defined(HAVE_STAT_TV_NSEC)
        ansec = st->st_atim.tv_nsec;
        mnsec = st->st_mtim.tv_nsec;
        cnsec = st->st_ctim.tv_nsec;
#elif defined(HAVE_STAT_TV_NSEC2)
        ansec = st->st_atimespec.tv_nsec;
        mnsec = st->st_mtimespec.tv_nsec;
        cnsec = st->st_ctimespec.tv_nsec;
#elif defined(HAVE_STAT_NSEC)
        ansec = st->st_atime_nsec;
        mnsec = st->st_mtime_nsec;
        cnsec = st->st_ctime_nsec;
#else
        ansec = mnsec = cnsec = 0;
#endif
        PyTuple_SET_ITEM(stat_data, 7, format_time(st->st_atime, ansec));
        PyTuple_SET_ITEM(stat_data, 8, format_time(st->st_mtime, mnsec));
        PyTuple_SET_ITEM(stat_data, 9, format_time(st->st_ctime, cnsec));
#ifdef HAVE_STRUCT_STAT_ST_BLKSIZE
        PyTuple_SET_ITEM(stat_data, 10, PyInt_FromLong((long)st->st_blksize));
# else
        Py_INCREF(Py_None);
        PyTuple_SET_ITEM(stat_data, 10, Py_None);
#endif
#ifdef HAVE_STRUCT_STAT_ST_BLOCKS
        PyTuple_SET_ITEM(stat_data, 11, PyInt_FromLong((long)st->st_blocks));
# else
        Py_INCREF(Py_None);
        PyTuple_SET_ITEM(stat_data, 11, Py_None);
#endif
#ifdef HAVE_STRUCT_STAT_ST_RDEV
        PyTuple_SET_ITEM(stat_data, 12, PyInt_FromLong((long)st->st_rdev));
# else
        Py_INCREF(Py_None);
        PyTuple_SET_ITEM(stat_data, 12, Py_None);
#endif
    }

    if (req_data->callback != Py_None) {
        if (!req_data->data) {
            Py_INCREF(Py_None);
            req_data->data = Py_None;
        }
        PyObject *cb_result = PyObject_CallFunctionObjArgs(req_data->callback, req_data->loop, req_data->data, result, errorno, stat_data, NULL);
        if (cb_result == NULL) {
            PyErr_WriteUnraisable(req_data->callback);
        }
        Py_XDECREF(cb_result);
    }

stat_end:

    Py_DECREF(req_data->loop);
    Py_XDECREF(req_data->callback);
    Py_XDECREF(req_data->data);
    PyMem_Free(req_data);
    req->data = NULL;
    uv_fs_req_cleanup(req);
    PyMem_Free(req);

    PyGILState_Release(gstate);
}


static PyObject *
stat_func(PyObject *self, PyObject *args, PyObject *kwargs, int type)
{
    uv_fs_t *fs_req;
    fs_req_data_t *req_data;
    int r;
    char *path;
    Loop *loop;
    PyObject *callback = NULL;
    PyObject *data = NULL;

    static char *kwlist[] = {"path", "loop", "callback", "data", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "sO!|OO:stat", kwlist, &path, &LoopType, &loop, &callback, &data)) {
        return NULL;
    }

    if (callback != Py_None && !PyCallable_Check(callback)) {
        PyErr_SetString(PyExc_TypeError, "a callable or None is required");
        return NULL;
    }

    fs_req = PyMem_Malloc(sizeof(uv_fs_t));
    if (!fs_req) {
        PyErr_NoMemory();
        return NULL;
    }
   
    req_data = PyMem_Malloc(sizeof(fs_req_data_t));
    if (!req_data) {
        PyErr_NoMemory();
        return NULL;
    }

    Py_INCREF(loop);
    Py_XINCREF(callback);
    Py_XINCREF(data);

    req_data->loop = loop;
    req_data->callback = callback;
    req_data->data = data;

    fs_req->data = (void *)req_data;
    if (type == UV_FS_STAT) {
        r = uv_fs_stat(loop->uv_loop, fs_req, path, stat_cb);
    } else {
        r = uv_fs_lstat(loop->uv_loop, fs_req, path, stat_cb);
    }
    if (r) {
        RAISE_ERROR(loop->uv_loop, PyExc_FSError, NULL);
    }

    Py_RETURN_NONE;
}


static PyObject *
FS_func_stat(PyObject *self, PyObject *args, PyObject *kwargs)
{
    return stat_func(self, args, kwargs, UV_FS_STAT);
}


static PyObject *
FS_func_lstat(PyObject *self, PyObject *args, PyObject *kwargs)
{
    return stat_func(self, args, kwargs, UV_FS_LSTAT);
}


static PyMethodDef
FS_methods[] = {
    { "stat", (PyCFunction)FS_func_stat, METH_VARARGS|METH_KEYWORDS, "stat" },
    { "lstat", (PyCFunction)FS_func_lstat, METH_VARARGS|METH_KEYWORDS, "lstat" },
    { NULL }
};

