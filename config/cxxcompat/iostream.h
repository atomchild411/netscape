/* Pre-standard <iostream.h> for today's C++ libraries: the 1998 tree
 * includes it and uses the stream names without std::. */
#ifndef CXXCOMPAT_IOSTREAM_H
#define CXXCOMPAT_IOSTREAM_H
#include <iostream>
using std::ios;
using std::istream;
using std::ostream;
using std::iostream;
using std::streambuf;
using std::cin;
using std::cout;
using std::cerr;
using std::endl;
using std::flush;
#endif
