/*[clinic input]
preserve
[clinic start generated code]*/

#if (PYSTON_SPEEDUPS)

static PyObject *
delta_new_impl(PyTypeObject *type, PyObject *day, PyObject *second,
               PyObject *us, PyObject *ms, PyObject *minute, PyObject *hour,
               PyObject *week);

static PyObject *
delta_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    PyObject *return_value = NULL;
    static const char * const _keywords[] = {"days", "seconds", "microseconds", "milliseconds", "minutes", "hours", "weeks", NULL};
    static _PyArg_Parser _parser = {NULL, _keywords, "delta", 0};
    PyObject *argsbuf[7];
    PyObject * const *fastargs;
    Py_ssize_t nargs = PyTuple_GET_SIZE(args);
    Py_ssize_t noptargs = nargs + (kwargs ? PyDict_GET_SIZE(kwargs) : 0) - 0;
    PyObject *day = NULL;
    PyObject *second = NULL;
    PyObject *us = NULL;
    PyObject *ms = NULL;
    PyObject *minute = NULL;
    PyObject *hour = NULL;
    PyObject *week = NULL;

    fastargs = _PyArg_UnpackKeywords(_PyTuple_CAST(args)->ob_item, nargs, kwargs, NULL, &_parser, 0, 7, 0, argsbuf);
    if (!fastargs) {
        goto exit;
    }
    if (!noptargs) {
        goto skip_optional_pos;
    }
    if (fastargs[0]) {
        day = fastargs[0];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[1]) {
        second = fastargs[1];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[2]) {
        us = fastargs[2];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[3]) {
        ms = fastargs[3];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[4]) {
        minute = fastargs[4];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[5]) {
        hour = fastargs[5];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    week = fastargs[6];
skip_optional_pos:
    return_value = delta_new_impl(type, day, second, us, ms, minute, hour, week);

exit:
    return return_value;
}

#endif /* (PYSTON_SPEEDUPS) */

PyDoc_STRVAR(datetime_date_fromtimestamp__doc__,
"fromtimestamp($type, timestamp, /)\n"
"--\n"
"\n"
"Create a date from a POSIX timestamp.\n"
"\n"
"The timestamp is a number, e.g. created via time.time(), that is interpreted\n"
"as local time.");

#define DATETIME_DATE_FROMTIMESTAMP_METHODDEF    \
    {"fromtimestamp", (PyCFunction)datetime_date_fromtimestamp, METH_O|METH_CLASS, datetime_date_fromtimestamp__doc__},

PyDoc_STRVAR(datetime_datetime_now__doc__,
"now($type, /, tz=None)\n"
"--\n"
"\n"
"Returns new datetime object representing current time local to tz.\n"
"\n"
"  tz\n"
"    Timezone object.\n"
"\n"
"If no tz is specified, uses local timezone.");

#define DATETIME_DATETIME_NOW_METHODDEF    \
    {"now", (PyCFunction)(void(*)(void))datetime_datetime_now, METH_FASTCALL|METH_KEYWORDS|METH_CLASS, datetime_datetime_now__doc__},

static PyObject *
datetime_datetime_now_impl(PyTypeObject *type, PyObject *tz);

static PyObject *
datetime_datetime_now(PyTypeObject *type, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PyObject *return_value = NULL;
    static const char * const _keywords[] = {"tz", NULL};
    static _PyArg_Parser _parser = {NULL, _keywords, "now", 0};
    PyObject *argsbuf[1];
    Py_ssize_t noptargs = nargs + (kwnames ? PyTuple_GET_SIZE(kwnames) : 0) - 0;
    PyObject *tz = Py_None;

    args = _PyArg_UnpackKeywords(args, nargs, NULL, kwnames, &_parser, 0, 1, 0, argsbuf);
    if (!args) {
        goto exit;
    }
    if (!noptargs) {
        goto skip_optional_pos;
    }
    tz = args[0];
skip_optional_pos:
    return_value = datetime_datetime_now_impl(type, tz);

exit:
    return return_value;
}
/*[clinic end generated code: output=69ec93b7351b43d8 input=a9049054013a1b77]*/
