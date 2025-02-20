Datadog C++ Tracing Library
===========================
```c++
#include <datadog/tracer.h>
#include <datadog/tracer_config.h>

#include <chrono>
#include <iostream>
#include <thread>

int main() {
    namespace dd = datadog::tracing;

    dd::TracerConfig config;
    config.service = "my-service";

    const auto validated_config = dd::finalize_config(config);
    if (!validated_config) {
        std::cerr << validated_config.error() << '\n';
        return 1;
    }

    dd::Tracer tracer{*validated_config};
    dd::SpanConfig options;
    
    options.name = "parent";
    dd::Span parent = tracer.create_span(options);

    std::this_thread::sleep_for(std::chrono::seconds(1));

    options.name = "child";
    dd::Span child = parent.create_child(options);
    child.set_tag("foo", "bar");

    std::this_thread::sleep_for(std::chrono::seconds(2));
}
```

Build
-----
### `cmake && make && make install` Style Build
Build this library from source using [CMake][1]. Installation places a shared
library and public headers into the appropriate system directories
(`/usr/local/[...]`), or to a specified installation prefix.

A recent version of CMake is required (3.24), which might not be in your
system's package manager. [bin/install-cmake](bin/install-cmake) is an installer
for a recent CMake.

dd-trace-cpp requires a C++17 compiler.

Here is how to install dd-trace-cpp into `.install/` within the source
repository.
```console
$ git clone 'https://github.com/datadog/dd-trace-cpp'
$ cd dd-trace-cpp
$ bin/install-cmake
$ mkdir .install
$ mkdir .build
$ cd .build
$ cmake -DCMAKE_INSTALL_PREFIX=../.install ..
$ make -j $(nproc)
$ make install
$ find ../.install -type d
```

To instead install into `/usr/local/`, omit the `.install` directory and the
`-DCMAKE_INSTALL_PREFIX=../.install` option.

Then, when building an executable that uses `dd-trace-cpp`, specify the path to
the installed headers using an appropriate `-I` option.  If the library was
installed into the default system directories, then the `-I` option is not
needed.
```console
$ c++ -I/path/to/dd-trace-cpp/.install/include -c -o my_app.o my_app.cpp
```

When linking an executable that uses `dd-trace-cpp`, specify linkage to the
built library using the `-ldd_trace_cpp` option and an appropriate `-L` option.
If the library was installed into the default system directories, then the `-L`
options is not needed. The `-ldd_trace_cpp` option is always needed.
```console
$ c++ -o my_app my_app.o -L/path/to/dd-trace-cpp/.install/lib -ldd_trace_cpp
```

Test
----
Pass `-DBUILD_TESTING=1` to `cmake` to include the unit tests in the build.

The resulting unit test executable is `test/tests` within the build directory.
```console
$ mkdir .build
$ cd .build
$ cmake -DBUILD_TESTING=1 ..
$ make -j $(nproc)
$ ./test/tests
```

Alternatively, [bin/test](bin/test) is provided for convenience.

The most recent code coverage report is available [here][2].

Contributing
------------
See the [contributing guidelines](CONTRIBUTING.md).

[1]: https://cmake.org/
[2]: https://datadog.github.io/dd-trace-cpp/datadog
