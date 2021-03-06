// { dg-do run }
// PR c++/9844

extern "C" void abort();

#define vector __attribute__((vector_size(16)))

class vector_holder
{
   char __attribute__((vector_size(16))) vec;
   char __attribute__((vector_size(16))) vec1;
public:
   operator __attribute__((vector_size(16))) short (void) {
     return (__attribute__((vector_size(16))) short) vec;
   }

   operator __attribute__((vector_size(16))) int (void) {
     return (__attribute__((vector_size(16))) int) vec1;
   }

   vector_holder () {
	vec = (__attribute__((vector_size(16))) char) {'a', 'b', 'c', 'd', 'a', 'b', 'c', 'd',
						       'a', 'b', 'c', 'd', 'a', 'b', 'c', 'd'};
	vec1 = (__attribute__((vector_size(16))) char) {'m', 'n', 'o', 'q', 'm', 'n', 'o', 'p',
							'm', 'n', 'o', 'q', 'm', 'n', 'o', 'p'};
   }
};

union u {
              char f[16];
              vector unsigned int v;
} data;


vector_holder vh;

int main()
{
  data.v = (__attribute__((vector_size(16))) short) vh;
  if (data.f[0] != 'a' || data.f[15] != 'd')
    abort(); 
  data.v = (__attribute__((vector_size(16))) int) vh;
  if (data.f[0] != 'm' || data.f[15] != 'p')
    abort(); 

  return 0;
}
