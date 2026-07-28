package com.besteffortlabs.kindletshell;

public class HarnessSelfTest {
    public static void main(String[] args) {
        TestSupport.check(1 + 1 == 2, "arithmetic");
        TestSupport.eq("a", "a", "string eq");
        TestSupport.done("HarnessSelfTest");
    }
}
