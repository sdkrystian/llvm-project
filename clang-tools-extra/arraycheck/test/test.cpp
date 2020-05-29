using simple_typedef = int;
using simple_ptr_typedef = simple_typedef *;
using simple_ref_typedef = simple_typedef &;
using function_typedef = int();
using function_ptr_typedef = function_typedef *;
using function_ref_typedef = function_typedef &;
using bounded_array_typedef = int[4];
using bounded_array_ptr_typedef = bounded_array_typedef *;
using bounded_array_ref_typedef = bounded_array_typedef &;
using unbounded_array_typedef = int[];
using unbounded_array_ptr_typedef = unbounded_array_typedef *;
using unbounded_array_ref_typedef = unbounded_array_typedef &;

template <typename T> using simple_typedef_template = T;
template <typename T> using simple_ptr_typedef_template = simple_typedef_template<T> *;
template <typename T> using simple_ref_typedef_template = simple_typedef_template<T> &;
template <typename T> using bounded_array_typedef_template = T[4];
template <typename T> using bounded_array_ptr_typedef_template = bounded_array_typedef_template<T> *;
template <typename T> using bounded_array_ref_typedef_template = bounded_array_typedef_template<T> &;
template <typename T> using unbounded_array_typedef_template = T[];
template <typename T> using unbounded_array_ptr_typedef_template = unbounded_array_typedef_template<T> *;
template <typename T> using unbounded_array_ref_typedef_template = unbounded_array_typedef_template<T> &;
template <typename T> using function_typedef_template = T();
template <typename T> using function_ptr_typedef_template = function_typedef_template<T> *;
template <typename T> using function_ref_typedef_template = function_typedef_template<T> &;

simple_typedef simple_v;
simple_ptr_typedef simple_ptr_v;
simple_ref_typedef simple_ref_v;
function_typedef function_v;
function_ptr_typedef function_ptr_v;
function_ref_typedef function_ref_v;
bounded_array_typedef bounded_array_v;
bounded_array_ptr_typedef bounded_array_ptr_v;
bounded_array_ref_typedef bounded_array_ref_v;
unbounded_array_typedef unbounded_array_v; // not allowed, all decltypes will fail
unbounded_array_ptr_typedef unbounded_array_ptr_v;
unbounded_array_ref_typedef unbounded_array_ref_v;

template <typename T> simple_typedef_template<T> simple_v_template;
template <typename T> simple_ptr_typedef_template<T> simple_ptr_v_template;
template <typename T> simple_ref_typedef_template<T> simple_ref_v_template;
template <typename T> function_typedef_template<T> function_v_template;
template <typename T> function_ptr_typedef_template<T> function_ptr_v_template;
template <typename T> function_ref_typedef_template<T> function_ref_v_template;
template <typename T> bounded_array_typedef_template<T> bounded_array_v_template;
template <typename T> bounded_array_ptr_typedef_template<T> bounded_array_ptr_v_template;
template <typename T> bounded_array_ref_typedef_template<T> bounded_array_ref_v_template;
template <typename T> unbounded_array_typedef_template<T> unbounded_array_v_template;
template <typename T> unbounded_array_ptr_typedef_template<T> unbounded_array_ptr_v_template;
template <typename T> unbounded_array_ref_typedef_template<T> unbounded_array_ref_v_template;

// -----------------------------------

// non-member functions

// non-template plain

void f_simple(int x);
void f_simple_ptr(int *x);
void f_simple_ref(int &x);
void f_function(int x());
void f_function_ptr(int (*x)());
void f_function_ref(int (&x)());
void f_bounded_array(int x[4]);
void f_bounded_array_ptr(int (*x)[4]);
void f_bounded_array_ref(int (&x)[4]);
void f_unbounded_array(int x[]);
void f_unbounded_array_ptr(int (*x)[]);
void f_unbounded_array_ref(int (&x)[]);

// non-template decltypes

void f_simple_v(decltype(simple_v) x);
void f_simple_ptr_v(decltype(simple_ptr_v) x);
void f_simple_ref_v(decltype(simple_ref_v) x);
void f_function_v(decltype(function_v) x);
void f_function_ptr_v(decltype(function_ptr_v) x);
void f_function_ref_v(decltype(function_ref_v) x);
void f_bounded_array_v(decltype(bounded_array_v) x);
void f_bounded_array_ptr_v(decltype(bounded_array_ptr_v) x);
void f_bounded_array_ref_v(decltype(bounded_array_ref_v) x);
void f_unbounded_array_v(decltype(unbounded_array_v) x);
void f_unbounded_array_ptr_v(decltype(unbounded_array_ptr_v) x);
void f_unbounded_array_ref_v(decltype(unbounded_array_ref_v) x);

// non-template typedefs

void f_simple_typedef(simple_typedef x);
void f_simple_ptr_typedef(simple_ptr_typedef x);
void f_simple_ref_typedef(simple_ref_typedef x);
void f_function_typedef(function_typedef x);
void f_function_ptr_typedef(function_ptr_typedef x);
void f_function_ref_typedef(function_ref_typedef x);
void f_bounded_array_typedef(bounded_array_typedef x);
void f_bounded_array_ptr_typedef(bounded_array_ptr_typedef x);
void f_bounded_array_ref_typedef(bounded_array_ref_typedef x);
void f_unbounded_array_typedef(unbounded_array_typedef x);
void f_unbounded_array_ptr_typedef(unbounded_array_ptr_typedef x);
void f_unbounded_array_ref_typedef(unbounded_array_ref_typedef x);

// template plain

template <typename T> void f_simple_template(T x);
template <typename T> void f_simple_ptr_template(T *x);
template <typename T> void f_simple_ref_template(T &x);
template <typename T> void f_function_template(T x());
template <typename T> void f_function_ptr_template(T (*x)());
template <typename T> void f_function_ref_template(T (&x)());
template <typename T> void f_bounded_array_template(T x[4]);
template <typename T> void f_bounded_array_ptr_template(T (*x)[4]);
template <typename T> void f_bounded_array_ref_template(T (&x)[4]);
template <typename T> void f_unbounded_array_template(T x[]);
template <typename T> void f_unbounded_array_ptr_template(T (*x)[]);
template <typename T> void f_unbounded_array_ref_template(T (&x)[]);

// template decltypes

template <typename T> void f_simple_v(decltype(simple_v_template<T>) x);
template <typename T> void f_simple_ptr_v(decltype(simple_ptr_v_template<T>) x);
template <typename T> void f_simple_ref_v(decltype(simple_ref_v_template<T>) x);
template <typename T> void f_function_v(decltype(function_v_template<T>) x);
template <typename T> void f_function_ptr_v(decltype(function_ptr_v_template<T>) x);
template <typename T> void f_function_ref_v(decltype(function_ref_v_template<T>) x);
template <typename T> void f_bounded_array_v(decltype(bounded_array_v_template<T>) x);
template <typename T> void f_bounded_array_ptr_v(decltype(bounded_array_ptr_v_template<T>) x);
template <typename T> void f_bounded_array_ref_v(decltype(bounded_array_ref_v_template<T>) x);
template <typename T> void f_unbounded_array_v(decltype(unbounded_array_v_template<T>) x);
template <typename T> void f_unbounded_array_ptr_v(decltype(unbounded_array_ptr_v_template<T>) x);
template <typename T> void f_unbounded_array_ref_v(decltype(unbounded_array_ref_v_template<T>) x);

// template typedefs

template <typename T> void f_simple_typedef_template(simple_typedef_template<T> x);
template <typename T> void f_simple_ptr_typedef_template(simple_ptr_typedef_template<T> x);
template <typename T> void f_simple_ref_typedef_template(simple_ref_typedef_template<T> x);
template <typename T> void f_function_typedef_template(function_typedef_template<T> x);
template <typename T> void f_function_ptr_typedef_template(function_ptr_typedef_template<T> x);
template <typename T> void f_function_ref_typedef_template(function_ref_typedef_template<T> x);
template <typename T> void f_bounded_array_typedef_template(bounded_array_typedef_template<T> x);
template <typename T> void f_bounded_array_ptr_typedef_template(bounded_array_ptr_typedef_template<T> x);
template <typename T> void f_bounded_array_ref_typedef_template(bounded_array_ref_typedef_template<T> x);
template <typename T> void f_unbounded_array_typedef_template(unbounded_array_typedef_template<T> x);
template <typename T> void f_unbounded_array_ptr_typedef_template(unbounded_array_ptr_typedef_template<T> x);
template <typename T> void f_unbounded_array_ref_typedef_template(unbounded_array_ref_typedef_template<T> x);

// -----------------------------------

// member functions of classes

class member {

// non-member functions

// non-template plain

void f_simple(int x);
void f_simple_ptr(int *x);
void f_simple_ref(int &x);
void f_function(int x());
void f_function_ptr(int (*x)());
void f_function_ref(int (&x)());
void f_bounded_array(int x[4]);
void f_bounded_array_ptr(int (*x)[4]);
void f_bounded_array_ref(int (&x)[4]);
void f_unbounded_array(int x[]);
void f_unbounded_array_ptr(int (*x)[]);
void f_unbounded_array_ref(int (&x)[]);

// non-template decltypes

void f_simple_v(decltype(simple_v) x);
void f_simple_ptr_v(decltype(simple_ptr_v) x);
void f_simple_ref_v(decltype(simple_ref_v) x);
void f_function_v(decltype(function_v) x);
void f_function_ptr_v(decltype(function_ptr_v) x);
void f_function_ref_v(decltype(function_ref_v) x);
void f_bounded_array_v(decltype(bounded_array_v) x);
void f_bounded_array_ptr_v(decltype(bounded_array_ptr_v) x);
void f_bounded_array_ref_v(decltype(bounded_array_ref_v) x);
void f_unbounded_array_v(decltype(unbounded_array_v) x);
void f_unbounded_array_ptr_v(decltype(unbounded_array_ptr_v) x);
void f_unbounded_array_ref_v(decltype(unbounded_array_ref_v) x);

// non-template typedefs

void f_simple_typedef(simple_typedef x);
void f_simple_ptr_typedef(simple_ptr_typedef x);
void f_simple_ref_typedef(simple_ref_typedef x);
void f_function_typedef(function_typedef x);
void f_function_ptr_typedef(function_ptr_typedef x);
void f_function_ref_typedef(function_ref_typedef x);
void f_bounded_array_typedef(bounded_array_typedef x);
void f_bounded_array_ptr_typedef(bounded_array_ptr_typedef x);
void f_bounded_array_ref_typedef(bounded_array_ref_typedef x);
void f_unbounded_array_typedef(unbounded_array_typedef x);
void f_unbounded_array_ptr_typedef(unbounded_array_ptr_typedef x);
void f_unbounded_array_ref_typedef(unbounded_array_ref_typedef x);

// template plain

template <typename T> void f_simple_template(T x);
template <typename T> void f_simple_ptr_template(T *x);
template <typename T> void f_simple_ref_template(T &x);
template <typename T> void f_function_template(T x());
template <typename T> void f_function_ptr_template(T (*x)());
template <typename T> void f_function_ref_template(T (&x)());
template <typename T> void f_bounded_array_template(T x[4]);
template <typename T> void f_bounded_array_ptr_template(T (*x)[4]);
template <typename T> void f_bounded_array_ref_template(T (&x)[4]);
template <typename T> void f_unbounded_array_template(T x[]);
template <typename T> void f_unbounded_array_ptr_template(T (*x)[]);
template <typename T> void f_unbounded_array_ref_template(T (&x)[]);

// template decltypes

template <typename T> void f_simple_v(decltype(simple_v_template<T>) x);
template <typename T> void f_simple_ptr_v(decltype(simple_ptr_v_template<T>) x);
template <typename T> void f_simple_ref_v(decltype(simple_ref_v_template<T>) x);
template <typename T> void f_function_v(decltype(function_v_template<T>) x);
template <typename T> void f_function_ptr_v(decltype(function_ptr_v_template<T>) x);
template <typename T> void f_function_ref_v(decltype(function_ref_v_template<T>) x);
template <typename T> void f_bounded_array_v(decltype(bounded_array_v_template<T>) x);
template <typename T> void f_bounded_array_ptr_v(decltype(bounded_array_ptr_v_template<T>) x);
template <typename T> void f_bounded_array_ref_v(decltype(bounded_array_ref_v_template<T>) x);
template <typename T> void f_unbounded_array_v(decltype(unbounded_array_v_template<T>) x);
template <typename T> void f_unbounded_array_ptr_v(decltype(unbounded_array_ptr_v_template<T>) x);
template <typename T> void f_unbounded_array_ref_v(decltype(unbounded_array_ref_v_template<T>) x);

// template typedefs

template <typename T> void f_simple_typedef_template(simple_typedef_template<T> x);
template <typename T> void f_simple_ptr_typedef_template(simple_ptr_typedef_template<T> x);
template <typename T> void f_simple_ref_typedef_template(simple_ref_typedef_template<T> x);
template <typename T> void f_function_typedef_template(function_typedef_template<T> x);
template <typename T> void f_function_ptr_typedef_template(function_ptr_typedef_template<T> x);
template <typename T> void f_function_ref_typedef_template(function_ref_typedef_template<T> x);
template <typename T> void f_bounded_array_typedef_template(bounded_array_typedef_template<T> x);
template <typename T> void f_bounded_array_ptr_typedef_template(bounded_array_ptr_typedef_template<T> x);
template <typename T> void f_bounded_array_ref_typedef_template(bounded_array_ref_typedef_template<T> x);
template <typename T> void f_unbounded_array_typedef_template(unbounded_array_typedef_template<T> x);
template <typename T> void f_unbounded_array_ptr_typedef_template(unbounded_array_ptr_typedef_template<T> x);
template <typename T> void f_unbounded_array_ref_typedef_template(unbounded_array_ref_typedef_template<T> x);

};

// -----------------------------------

// member functions of class templates

template <typename T> class member_template {

// non-member functions

// non-template plain

void f_simple(int x);
void f_simple_ptr(int *x);
void f_simple_ref(int &x);
void f_function(int x());
void f_function_ptr(int (*x)());
void f_function_ref(int (&x)());
void f_bounded_array(int x[4]);
void f_bounded_array_ptr(int (*x)[4]);
void f_bounded_array_ref(int (&x)[4]);
void f_unbounded_array(int x[]);
void f_unbounded_array_ptr(int (*x)[]);
void f_unbounded_array_ref(int (&x)[]);

// non-template decltypes

void f_simple_v(decltype(simple_v) x);
void f_simple_ptr_v(decltype(simple_ptr_v) x);
void f_simple_ref_v(decltype(simple_ref_v) x);
void f_function_v(decltype(function_v) x);
void f_function_ptr_v(decltype(function_ptr_v) x);
void f_function_ref_v(decltype(function_ref_v) x);
void f_bounded_array_v(decltype(bounded_array_v) x);
void f_bounded_array_ptr_v(decltype(bounded_array_ptr_v) x);
void f_bounded_array_ref_v(decltype(bounded_array_ref_v) x);
void f_unbounded_array_v(decltype(unbounded_array_v) x);
void f_unbounded_array_ptr_v(decltype(unbounded_array_ptr_v) x);
void f_unbounded_array_ref_v(decltype(unbounded_array_ref_v) x);

// non-template typedefs

void f_simple_typedef(simple_typedef x);
void f_simple_ptr_typedef(simple_ptr_typedef x);
void f_simple_ref_typedef(simple_ref_typedef x);
void f_function_typedef(function_typedef x);
void f_function_ptr_typedef(function_ptr_typedef x);
void f_function_ref_typedef(function_ref_typedef x);
void f_bounded_array_typedef(bounded_array_typedef x);
void f_bounded_array_ptr_typedef(bounded_array_ptr_typedef x);
void f_bounded_array_ref_typedef(bounded_array_ref_typedef x);
void f_unbounded_array_typedef(unbounded_array_typedef x);
void f_unbounded_array_ptr_typedef(unbounded_array_ptr_typedef x);
void f_unbounded_array_ref_typedef(unbounded_array_ref_typedef x);

// template plain

template <typename T> void f_simple_template(T x);
template <typename T> void f_simple_ptr_template(T *x);
template <typename T> void f_simple_ref_template(T &x);
template <typename T> void f_function_template(T x());
template <typename T> void f_function_ptr_template(T (*x)());
template <typename T> void f_function_ref_template(T (&x)());
template <typename T> void f_bounded_array_template(T x[4]);
template <typename T> void f_bounded_array_ptr_template(T (*x)[4]);
template <typename T> void f_bounded_array_ref_template(T (&x)[4]);
template <typename T> void f_unbounded_array_template(T x[]);
template <typename T> void f_unbounded_array_ptr_template(T (*x)[]);
template <typename T> void f_unbounded_array_ref_template(T (&x)[]);

// template decltypes

template <typename T> void f_simple_v(decltype(simple_v_template<T>) x);
template <typename T> void f_simple_ptr_v(decltype(simple_ptr_v_template<T>) x);
template <typename T> void f_simple_ref_v(decltype(simple_ref_v_template<T>) x);
template <typename T> void f_function_v(decltype(function_v_template<T>) x);
template <typename T> void f_function_ptr_v(decltype(function_ptr_v_template<T>) x);
template <typename T> void f_function_ref_v(decltype(function_ref_v_template<T>) x);
template <typename T> void f_bounded_array_v(decltype(bounded_array_v_template<T>) x);
template <typename T> void f_bounded_array_ptr_v(decltype(bounded_array_ptr_v_template<T>) x);
template <typename T> void f_bounded_array_ref_v(decltype(bounded_array_ref_v_template<T>) x);
template <typename T> void f_unbounded_array_v(decltype(unbounded_array_v_template<T>) x);
template <typename T> void f_unbounded_array_ptr_v(decltype(unbounded_array_ptr_v_template<T>) x);
template <typename T> void f_unbounded_array_ref_v(decltype(unbounded_array_ref_v_template<T>) x);

// template typedefs

template <typename T> void f_simple_typedef_template(simple_typedef_template<T> x);
template <typename T> void f_simple_ptr_typedef_template(simple_ptr_typedef_template<T> x);
template <typename T> void f_simple_ref_typedef_template(simple_ref_typedef_template<T> x);
template <typename T> void f_function_typedef_template(function_typedef_template<T> x);
template <typename T> void f_function_ptr_typedef_template(function_ptr_typedef_template<T> x);
template <typename T> void f_function_ref_typedef_template(function_ref_typedef_template<T> x);
template <typename T> void f_bounded_array_typedef_template(bounded_array_typedef_template<T> x);
template <typename T> void f_bounded_array_ptr_typedef_template(bounded_array_ptr_typedef_template<T> x);
template <typename T> void f_bounded_array_ref_typedef_template(bounded_array_ref_typedef_template<T> x);
template <typename T> void f_unbounded_array_typedef_template(unbounded_array_typedef_template<T> x);
template <typename T> void f_unbounded_array_ptr_typedef_template(unbounded_array_ptr_typedef_template<T> x);
template <typename T> void f_unbounded_array_ref_typedef_template(unbounded_array_ref_typedef_template<T> x);

};

// -----------------------------------

// templates

// plain

template <int x> class t_simple;
template <int *x> class t_simple_ptr;
template <int &x> class t_simple_ref;
template <int x()> class t_function;
template <int (*x)()> class t_function_ptr;
template <int (&x)()> class t_function_ref;
template <int x[4]> class t_bounded_array;
template <int (*x)[4]> class t_bounded_array_ptr;
template <int (&x)[4]> class t_bounded_array_ref;
template <int x[]> class t_unbounded_array;
template <int (*x)[]> class t_unbounded_array_ptr;
template <int (&x)[]> class t_unbounded_array_ref;

// decltypes

template <decltype(simple_v) x> class t_simple_v;
template <decltype(simple_ptr_v) x> class t_simple_ptr_v;
template <decltype(simple_ref_v) x> class t_simple_ref_v;
template <decltype(function_v) x> class t_function_v;
template <decltype(function_ptr_v) x> class t_function_ptr_v;
template <decltype(function_ref_v) x> class t_function_ref_v;
template <decltype(bounded_array_v) x> class t_bounded_array_v;
template <decltype(bounded_array_ptr_v) x> class t_bounded_array_ptr_v;
template <decltype(bounded_array_ref_v) x> class t_bounded_array_ref_v;
template <decltype(unbounded_array_v) x> class t_unbounded_array_v;
template <decltype(unbounded_array_ptr_v) x> class t_unbounded_array_ptr_v;
template <decltype(unbounded_array_ref_v) x> class t_unbounded_array_ref_v;

// typedefs

template <simple_typedef x> class t_simple_typedef;
template <simple_ptr_typedef x> class t_simple_ptr_typedef;
template <simple_ref_typedef x> class t_simple_ref_typedef;
template <function_typedef x> class t_function_typedef;
template <function_ptr_typedef x> class t_function_ptr_typedef;
template <function_ref_typedef x> class t_function_ref_typedef;
template <bounded_array_typedef x> class t_bounded_array_typedef;
template <bounded_array_ptr_typedef x> class t_bounded_array_ptr_typedef;
template <bounded_array_ref_typedef x> class t_bounded_array_ref_typedef;
template <unbounded_array_typedef x> class t_unbounded_array_typedef;
template <unbounded_array_ptr_typedef x> class t_unbounded_array_ptr_typedef;
template <unbounded_array_ref_typedef x> class t_unbounded_array_ref_typedef;

/*  Correct results:

Function: int [4]
Function: int []
Function: decltype(bounded_array_v)
Function: bounded_array_typedef
Function: unbounded_array_typedef
Function: T [4]
Function: T []
Function: bounded_array_typedef_template<T>
Function: unbounded_array_typedef_template<T>
Function: int [4]
Function: int []
Function: decltype(bounded_array_v)
Function: bounded_array_typedef
Function: unbounded_array_typedef
Function: T [4]
Function: T []
Function: bounded_array_typedef_template<T>
Function: unbounded_array_typedef_template<T>
Function: int [4]
Function: int []
Function: decltype(bounded_array_v)
Function: bounded_array_typedef
Function: unbounded_array_typedef
Function: T [4]
Function: T []
Function: bounded_array_typedef_template<T>
Function: unbounded_array_typedef_template<T>
NTTP: int [4]
NTTP: int []
NTTP: decltype(bounded_array_v)
NTTP: bounded_array_typedef
NTTP: unbounded_array_typedef

*/