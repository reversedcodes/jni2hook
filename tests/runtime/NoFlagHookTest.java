/* Whether a hook can reach native code without adding a method to the class it
   hooks -- and so without AllowRedefinitionToAddDeleteMethods -- and whether the
   hooked method's own arguments come along.

   Both targets are called before and after the rewrite, so the answer covers
   the whole path: the VM accepted the redefinition, the inserted call arrives in
   native code carrying its arguments, and the original body still produces the
   value it always did. */
public class NoFlagHookTest {

    static native int run(Class<?> target);
    static native int plainCalls();
    static native int plainId();
    static native int mixedCalls();
    static native int mixedId();
    static native int mixedInt();
    static native long mixedLong();
    static native double mixedDouble();
    static native boolean mixedReceiverArrived();
    static native boolean mixedTextArrived();
    static native void shutdown();

    static int failures = 0;

    static void check(String what, Object actual, Object expected) {
        boolean ok = expected.equals(actual);
        System.out.printf("  %-46s = %-18s %s%n", what, actual,
                          ok ? "ok" : "FAIL expected " + expected);
        if (!ok) failures++;
    }

    static void check(String what, boolean condition) {
        System.out.printf("  %-60s %s%n", what, condition ? "ok" : "FAIL");
        if (!condition) failures++;
    }

    public static int compute(int value) {
        return (value * 7) + 3;
    }

    public int mixed(int number, String text, long big, double fraction) {
        return number + text.length() + (int) big + (int) fraction;
    }

    public static void main(String[] args) {
        System.load(args[0]);

        check("the targets work before anything happened",
              compute(6) == 45 && new NoFlagHookTest().mixed(1, "hello", 2L, 3.0) == 11);

        failures += run(NoFlagHookTest.class);

        System.out.println("\n-- a static method, identifier only --");
        check("compute(6) still returns its value", compute(6), 45);
        check("the call reached native", plainCalls(), 1);
        check("it carried its identifier", plainId(), 4242);

        System.out.println("\n-- an instance method, every argument forwarded --");
        NoFlagHookTest instance = new NoFlagHookTest();
        check("mixed(...) still returns its value", instance.mixed(9, "hello", 100L, 2.5), 116);
        check("the call reached native", mixedCalls(), 1);
        check("identifier", mixedId(), 77);
        check("the receiver arrived as an object", mixedReceiverArrived());
        check("int argument", mixedInt(), 9);
        check("String argument, read back through JNI", mixedTextArrived());
        check("long argument", mixedLong(), 100L);
        check("double argument", mixedDouble(), 2.5);

        instance.mixed(1, "hello", 2L, 3.0);
        check("and again on the next call", mixedCalls(), 2);

        shutdown();

        System.out.println(failures == 0
            ? "\na hook reaches native with its arguments, without adding a method,"
              + " and so without the flag"
            : "\n" + failures + " CHECKS FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
