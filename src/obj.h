#pragma once

#include "common.h"
#include "namedict.h"
#include "tuplelist.h"

namespace pkpy {
    
struct CodeObject;
struct Frame;
struct Function;
class VM;

#if PK_ENABLE_STD_FUNCTION
using NativeFuncC = std::function<PyObject*(VM*, ArgsView)>;
#else
typedef PyObject* (*NativeFuncC)(VM*, ArgsView);
#endif

typedef int (*LuaStyleFuncC)(VM*);

struct NativeFunc {
    NativeFuncC f;
    int argc;

    // this is designed for lua style C bindings
    LuaStyleFuncC _lua_f;

    using UserData = char[32];
    UserData _userdata;
    bool _has_userdata;

    template <typename T>
    void set_userdata(T data) {
        static_assert(std::is_trivially_copyable_v<T>);
        static_assert(sizeof(T) <= sizeof(UserData));
        if(_has_userdata) throw std::runtime_error("userdata already set");
        _has_userdata = true;
        memcpy(_userdata, &data, sizeof(T));
    }

    template <typename T>
    T get_userdata() const {
        static_assert(std::is_trivially_copyable_v<T>);
        static_assert(sizeof(T) <= sizeof(UserData));
#if DEBUG_EXTRA_CHECK
        if(!_has_userdata) throw std::runtime_error("userdata not set");
#endif
        return reinterpret_cast<const T&>(_userdata);
    }
    
    NativeFunc(NativeFuncC f, int argc, bool method){
        this->f = f;
        this->argc = argc;
        if(argc != -1) this->argc += (int)method;
        _lua_f = nullptr;
        _has_userdata = false;
    }

    PyObject* operator()(VM* vm, ArgsView args) const;
};

typedef shared_ptr<CodeObject> CodeObject_;

struct FuncDecl {
    struct KwArg {
        int key;                // index in co->varnames
        PyObject* value;        // default value
    };
    CodeObject_ code;           // code object of this function
    pod_vector<int> args;       // indices in co->varnames
    pod_vector<KwArg> kwargs;   // indices in co->varnames
    int starred_arg = -1;       // index in co->varnames, -1 if no *arg
    int starred_kwarg = -1;     // index in co->varnames, -1 if no **kwarg
    bool nested = false;        // whether this function is nested
    void _gc_mark() const;
};

using FuncDecl_ = shared_ptr<FuncDecl>;

struct Function{
    FuncDecl_ decl;
    bool is_simple;
    int argc;   // cached argc
    PyObject* _module;
    NameDict_ _closure;
};

struct BoundMethod {
    PyObject* self;
    PyObject* func;
    BoundMethod(PyObject* self, PyObject* func) : self(self), func(func) {}
    
    bool operator==(const BoundMethod& rhs) const noexcept {
        return self == rhs.self && func == rhs.func;
    }
    bool operator!=(const BoundMethod& rhs) const noexcept {
        return self != rhs.self || func != rhs.func;
    }
};

struct Property{
    PyObject* getter;
    PyObject* setter;
    Property(PyObject* getter, PyObject* setter) : getter(getter), setter(setter) {}
};

struct Range {
    i64 start = 0;
    i64 stop = -1;
    i64 step = 1;
};

struct StarWrapper{
    int level;      // either 1 or 2
    PyObject* obj;
    StarWrapper(int level, PyObject* obj) : level(level), obj(obj) {}
};

struct Bytes{
    std::vector<char> _data;
    bool _ok;

    int size() const noexcept { return _data.size(); }
    int operator[](int i) const noexcept { return (int)(uint8_t)_data[i]; }
    const char* data() const noexcept { return _data.data(); }

    bool operator==(const Bytes& rhs) const noexcept {
        return _data == rhs._data;
    }
    bool operator!=(const Bytes& rhs) const noexcept {
        return _data != rhs._data;
    }

    std::string str() const noexcept { return std::string(_data.begin(), _data.end()); }

    Bytes() : _data(), _ok(false) {}
    Bytes(std::vector<char>&& data) : _data(std::move(data)), _ok(true) {}
    Bytes(const std::string& data) : _data(data.begin(), data.end()), _ok(true) {}
    operator bool() const noexcept { return _ok; }
};

using Super = std::pair<PyObject*, Type>;

struct Slice {
    PyObject* start;
    PyObject* stop;
    PyObject* step;
    Slice(PyObject* start, PyObject* stop, PyObject* step) : start(start), stop(stop), step(step) {}
};

struct GCHeader {
    bool enabled;   // whether this object is managed by GC
    bool marked;    // whether this object is marked
    GCHeader() : enabled(true), marked(false) {}
};

struct PyObject{
    GCHeader gc;
    Type type;
    NameDict* _attr;

    bool is_attr_valid() const noexcept { return _attr != nullptr; }
    NameDict& attr() noexcept { return *_attr; }
    PyObject* attr(StrName name) const noexcept { return (*_attr)[name]; }
    virtual void _obj_gc_mark() = 0;

    PyObject(Type type) : type(type), _attr(nullptr) {}

    virtual ~PyObject() {
        if(_attr == nullptr) return;
        _attr->~NameDict();
        pool64.dealloc(_attr);
    }

    void enable_instance_dict(float lf=kInstAttrLoadFactor) noexcept {
        _attr = new(pool64.alloc<NameDict>()) NameDict(lf);
    }
};

template <typename, typename=void> struct has_gc_marker : std::false_type {};
template <typename T> struct has_gc_marker<T, std::void_t<decltype(&T::_gc_mark)>> : std::true_type {};

template <typename T>
struct Py_ final: PyObject {
    T _value;
    void _obj_gc_mark() override {
        if constexpr (has_gc_marker<T>::value) {
            _value._gc_mark();
        }
    }
    Py_(Type type, const T& value) : PyObject(type), _value(value) {}
    Py_(Type type, T&& value) : PyObject(type), _value(std::move(value)) {}
};

struct MappingProxy{
    PyObject* obj;
    MappingProxy(PyObject* obj) : obj(obj) {}
    NameDict& attr() noexcept { return obj->attr(); }
};

#define OBJ_GET(T, obj) (((Py_<T>*)(obj))->_value)

#define OBJ_MARK(obj) \
    if(!is_tagged(obj) && !(obj)->gc.marked) {                      \
        (obj)->gc.marked = true;                                    \
        (obj)->_obj_gc_mark();                                      \
        if((obj)->is_attr_valid()) gc_mark_namedict((obj)->attr()); \
    }

inline void gc_mark_namedict(NameDict& t){
    if(t.size() == 0) return;
    for(uint16_t i=0; i<t._capacity; i++){
        if(t._items[i].first.empty()) continue;
        OBJ_MARK(t._items[i].second);
    }
}

Str obj_type_name(VM* vm, Type type);

#if DEBUG_NO_BUILTIN_MODULES
#define OBJ_NAME(obj) Str("<?>")
#else
DEF_SNAME(__name__);
#define OBJ_NAME(obj) OBJ_GET(Str, vm->getattr(obj, __name__))
#endif

const int kTpIntIndex = 2;
const int kTpFloatIndex = 3;

inline bool is_type(PyObject* obj, Type type) {
#if DEBUG_EXTRA_CHECK
    if(obj == nullptr) throw std::runtime_error("is_type() called with nullptr");
    if(is_special(obj)) throw std::runtime_error("is_type() called with special object");
#endif
    switch(type.index){
        case kTpIntIndex: return is_int(obj);
        case kTpFloatIndex: return is_float(obj);
        default: return !is_tagged(obj) && obj->type == type;
    }
}

inline bool is_non_tagged_type(PyObject* obj, Type type) {
#if DEBUG_EXTRA_CHECK
    if(obj == nullptr) throw std::runtime_error("is_non_tagged_type() called with nullptr");
    if(is_special(obj)) throw std::runtime_error("is_non_tagged_type() called with special object");
#endif
    return !is_tagged(obj) && obj->type == type;
}

union BitsCvt {
    i64 _int;
    f64 _float;
    BitsCvt(i64 val) : _int(val) {}
    BitsCvt(f64 val) : _float(val) {}
};

template <typename, typename=void> struct is_py_class : std::false_type {};
template <typename T> struct is_py_class<T, std::void_t<decltype(T::_type)>> : std::true_type {};

template<typename T> T to_void_p(VM*, PyObject*);
template<typename T> T to_c99_struct(VM*, PyObject*);

template<typename __T>
__T py_cast(VM* vm, PyObject* obj) {
    using T = std::decay_t<__T>;
    if constexpr(std::is_enum_v<T>){
        return (__T)py_cast<i64>(vm, obj);
    }else if constexpr(std::is_pointer_v<T>){
        return to_void_p<T>(vm, obj);
    }else if constexpr(is_py_class<T>::value){
        T::_check_type(vm, obj);
        return OBJ_GET(T, obj);
    }else if constexpr(std::is_pod_v<T>){
        return to_c99_struct<T>(vm, obj);
    }else {
        return Discarded();
    }
}

template<typename __T>
__T _py_cast(VM* vm, PyObject* obj) {
    using T = std::decay_t<__T>;
    if constexpr(std::is_enum_v<T>){
        return (__T)_py_cast<i64>(vm, obj);
    }else if constexpr(std::is_pointer_v<__T>){
        return to_void_p<__T>(vm, obj);
    }else if constexpr(is_py_class<T>::value){
        return OBJ_GET(T, obj);
    }else if constexpr(std::is_pod_v<T>){
        return to_c99_struct<T>(vm, obj);
    }else {
        return Discarded();
    }
}

#define VAR(x) py_var(vm, x)
#define CAST(T, x) py_cast<T>(vm, x)
#define _CAST(T, x) _py_cast<T>(vm, x)

#define CAST_F(x) vm->num_to_float(x)

/*****************************************************************/
template<>
struct Py_<List> final: PyObject {
    List _value;
    Py_(Type type, List&& val): PyObject(type), _value(std::move(val)) {}
    Py_(Type type, const List& val): PyObject(type), _value(val) {}

    void _obj_gc_mark() override {
        for(PyObject* obj: _value) OBJ_MARK(obj);
    }
};

template<>
struct Py_<Tuple> final: PyObject {
    Tuple _value;
    Py_(Type type, Tuple&& val): PyObject(type), _value(std::move(val)) {}
    Py_(Type type, const Tuple& val): PyObject(type), _value(val) {}

    void _obj_gc_mark() override {
        for(PyObject* obj: _value) OBJ_MARK(obj);
    }
};

template<>
struct Py_<MappingProxy> final: PyObject {
    MappingProxy _value;
    Py_(Type type, MappingProxy val): PyObject(type), _value(val) {}
    void _obj_gc_mark() override {
        OBJ_MARK(_value.obj);
    }
};

template<>
struct Py_<BoundMethod> final: PyObject {
    BoundMethod _value;
    Py_(Type type, BoundMethod val): PyObject(type), _value(val) {}
    void _obj_gc_mark() override {
        OBJ_MARK(_value.self);
        OBJ_MARK(_value.func);
    }
};

template<>
struct Py_<StarWrapper> final: PyObject {
    StarWrapper _value;
    Py_(Type type, StarWrapper val): PyObject(type), _value(val) {}
    void _obj_gc_mark() override {
        OBJ_MARK(_value.obj);
    }
};

template<>
struct Py_<Property> final: PyObject {
    Property _value;
    Py_(Type type, Property val): PyObject(type), _value(val) {}
    void _obj_gc_mark() override {
        OBJ_MARK(_value.getter);
        OBJ_MARK(_value.setter);
    }
};

template<>
struct Py_<Slice> final: PyObject {
    Slice _value;
    Py_(Type type, Slice val): PyObject(type), _value(val) {}
    void _obj_gc_mark() override {
        OBJ_MARK(_value.start);
        OBJ_MARK(_value.stop);
        OBJ_MARK(_value.step);
    }
};

template<>
struct Py_<Function> final: PyObject {
    Function _value;
    Py_(Type type, Function val): PyObject(type), _value(val) {
        enable_instance_dict();
    }
    void _obj_gc_mark() override {
        _value.decl->_gc_mark();
        if(_value._module != nullptr) OBJ_MARK(_value._module);
        if(_value._closure != nullptr) gc_mark_namedict(*_value._closure);
    }
};

template<>
struct Py_<NativeFunc> final: PyObject {
    NativeFunc _value;
    Py_(Type type, NativeFunc val): PyObject(type), _value(val) {
        enable_instance_dict();
    }
    void _obj_gc_mark() override {}
};

template<>
struct Py_<Super> final: PyObject {
    Super _value;
    Py_(Type type, Super val): PyObject(type), _value(val) {}
    void _obj_gc_mark() override {
        OBJ_MARK(_value.first);
    }
};

template<>
struct Py_<DummyInstance> final: PyObject {
    Py_(Type type, DummyInstance val): PyObject(type) {
        enable_instance_dict();
    }
    void _obj_gc_mark() override {}
};

template<>
struct Py_<Type> final: PyObject {
    Type _value;
    Py_(Type type, Type val): PyObject(type), _value(val) {
        enable_instance_dict(kTypeAttrLoadFactor);
    }
    void _obj_gc_mark() override {}
};

template<>
struct Py_<DummyModule> final: PyObject {
    Py_(Type type, DummyModule val): PyObject(type) {
        enable_instance_dict(kTypeAttrLoadFactor);
    }
    void _obj_gc_mark() override {}
};

template<typename T>
inline T lambda_get_userdata(PyObject** p){
    if(p[-1] != PY_NULL) return OBJ_GET(NativeFunc, p[-1]).get_userdata<T>();
    else return OBJ_GET(NativeFunc, p[-2]).get_userdata<T>();
}



}   // namespace pkpy