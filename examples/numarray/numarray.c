#include <stdlib.h>
#include <bender.h>

typedef struct {
    double *data;
    size_t size;
} num_array;

static num_array *num_array_init(num_array *array, size_t size) {
    array->data = (double *)bender_calloc(size, sizeof(double));
    array->size = size;
    return array;
}

static void num_array_deinit(num_array *array) {
    bender_free(array->data);
}

static int num_array_gc(void *p, size_t s) {
    (void) s;
    num_array *array = (num_array *)p;
    num_array_deinit(array);
    return 0;
}

int num_array_get(void *p, Bender key, Bender *out);
void num_array_put(void *p, Bender key, Bender value);

static const BenderAbstractType num_array_type = {
    "numarray",
    num_array_gc,
    NULL,
    num_array_get,
    num_array_put,
    BENDER_ATEND_PUT
};

static Bender num_array_new(int32_t argc, Bender *argv) {
    bender_fixarity(argc, 1);
    int32_t size = bender_getinteger(argv, 0);
    num_array *array = (num_array *)bender_abstract(&num_array_type, sizeof(num_array));
    num_array_init(array, size);
    return bender_wrap_abstract(array);
}

static Bender num_array_scale(int32_t argc, Bender *argv) {
    bender_fixarity(argc, 2);
    num_array *array = (num_array *)bender_getabstract(argv, 0, &num_array_type);
    double factor = bender_getnumber(argv, 1);
    size_t i;
    for (i = 0; i < array->size; i++) {
        array->data[i] *= factor;
    }
    return argv[0];
}

static Bender num_array_sum(int32_t argc, Bender *argv) {
    bender_fixarity(argc, 1);
    num_array *array = (num_array *)bender_getabstract(argv, 0, &num_array_type);
    double sum = 0;
    for (size_t i = 0; i < array->size; i++) sum += array->data[i];
    return bender_wrap_number(sum);
}

void num_array_put(void *p, Bender key, Bender value) {
    size_t index;
    num_array *array = (num_array *)p;
    if (!bender_checkint(key))
        bender_panic("expected integer key");
    if (!bender_checktype(value, BENDER_NUMBER))
        bender_panic("expected number value");

    index = (size_t)bender_unwrap_integer(key);
    if (index < array->size) {
        array->data[index] = bender_unwrap_number(value);
    }
}

static Bender num_array_length(int32_t argc, Bender *argv) {
    bender_fixarity(argc, 1);
    num_array *array = (num_array *)bender_getabstract(argv, 0, &num_array_type);
    return bender_wrap_number(array->size);
}

static const BenderMethod methods[] = {
    {"scale", num_array_scale},
    {"sum", num_array_sum},
    {"length", num_array_length},
    {NULL, NULL}
};

int num_array_get(void *p, Bender key, Bender *out) {
    size_t index;
    num_array *array = (num_array *)p;
    if (bender_checktype(key, BENDER_KEYWORD))
        return bender_getmethod(bender_unwrap_keyword(key), methods, out);
    if (!bender_checkint(key))
        bender_panic("expected integer key");
    index = (size_t)bender_unwrap_integer(key);
    if (index >= array->size) {
        return 0;
    } else {
        *out = bender_wrap_number(array->data[index]);
    }
    return 1;
}

static const BenderReg cfuns[] = {
    {
        "new", num_array_new,
        "(numarray/new size)\n\n"
        "Create new numarray"
    },
    {
        "scale", num_array_scale,
        "(numarray/scale numarray factor)\n\n"
        "scale numarray by factor"
    },
    {
        "sum", num_array_sum,
        "(numarray/sum numarray)\n\n"
        "sums numarray"
    },
    {NULL, NULL, NULL}
};

BENDER_MODULE_ENTRY(BenderTable *env) {
    bender_cfuns(env, "numarray", cfuns);
}
