/* #pragma D option amin=Stable/Stable/CPU */
/* #pragma D attributes Internal/Internal/Unknown provider EditlineTest* */

EditlineTest*:EditlineTests::testsuite-setup-entry
/execname=="EditlineTests"/
  {
    self->ts = timestamp
  }

EditlineTest*:EditlineTests::testsuite-setup-return
/execname=="EditlineTests"/
  {
    @["testsuite-setup"] = quantize(timestamp - self->ts)
  }


EditlineTest*:EditlineTests::testcase-setup-entry
/execname=="EditlineTests"/
  {
    self->ts = timestamp
  }

EditlineTest*:EditlineTests::testcase-setup-return
/execname=="EditlineTests"/
  {
    @["testcase-setup"] = quantize(timestamp - self->ts)
  }


EditlineTest*:EditlineTests::test-multiline-entry
/execname=="EditlineTests"/
  {
    self->ts = timestamp;

  }

EditlineTest*:EditlineTests::test-multiline-return
/execname=="EditlineTests"/
  {
    @[args[0]] = quantize(timestamp - self->ts);

  }
