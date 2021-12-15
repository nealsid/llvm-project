# Instrumenting unit tests with DTrace

This branch has a few changes to experiment with using DTrace to look at unit test performance.

* [Probe definitions](lldb/unittests/Editline/EditlineTest.d)
* [D program](lldb/unittests/Editline/EditlinePerf.d)
* [Instrumented unit test file](lldb/unittests/Editline/EditlineTest.cpp)

To run (assuming you are in a build tree that is a sibling of the source tree, that you've already run CMake, and that you're using the Ninja generator):

```
$ dtrace -arch x86_64 -h -s ../llvm-project/lldb/unittests/Editline/EditlineTest.d \
      -o ../llvm-project/lldb/unittests/Editline/EditlineTest.h
$ ninja EditlineTests && sudo dtrace -c tools/lldb/unittests/Editline/EditlineTests \
    -s ../llvm-project/lldb/unittests/Editline/EditlinePerf.d
```

I think there's more useful information to be gotten from the performance counters available through Instruments.  For instance, it was clear that a great deal of time was spent in the EditlineAdapter destructor, which I traced to the command history loading/saving (from a file in the user's home directory).  Removing this from unit tests, which it isn't necessary for, gave a nice speedup (from a few ms/test to about 1 ms on my Xeon iMac Pro).
