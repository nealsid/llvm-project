/* dtrace -arch i386 -h -s ../llvm-project/lldb/unittests/Editline/EditlineTest.d  -o ../llvm-project/lldb/unittests/Editline/EditlineTest.h */

provider EditlineTest {
  probe testsuite__setup__entry();
  probe testsuite__setup__return();
  probe testsuite__teardown__entry();
  probe testsuite__teardown__return();
  probe testcase__setup__entry();
  probe testcase__setup__return();
  probe testcase__teardown__entry();
  probe testcase__teardown__return();
  probe test__singleline__entry();
  probe test__singleline__return();
  probe test__multiline__entry(char*);
  probe test__multiline__return(char*);
};
