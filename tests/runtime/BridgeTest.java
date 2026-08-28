/* Whether the object work a hook does can move out of JNI and into plain Java.

   HookBridge is deliberately kept off the class path: if it were loadable at
   startup the test would prove nothing. It is compiled to a separate directory,
   read as bytes and defined into the target's own class loader, which is what
   lets it name BridgeTarget and work with the receiver as a typed object. */
public class BridgeTest {

    static native int run(Class<?> target, String bridgeClassFile);
    static native int reports();
    static native int reportedId();
    static native int reportedValue();
    static native String reportedLabel();
    static native void shutdown();

    static int failures = 0;

    static void check(String what, Object actual, Object expected) {
        boolean ok = expected.equals(actual);
        System.out.printf("  %-46s = %-16s %s%n", what, actual,
                          ok ? "ok" : "FAIL expected " + expected);
        if (!ok) failures++;
    }

    static void check(String what, boolean condition) {
        System.out.printf("  %-60s %s%n", what, condition ? "ok" : "FAIL");
        if (!condition) failures++;
    }

    public static void main(String[] args) throws Exception {
        System.load(args[0]);

        boolean absent = false;
        try {
            Class.forName("HookBridge");
        } catch (ClassNotFoundException expected) {
            absent = true;
        }
        check("HookBridge is not on the class path to begin with", absent);

        BridgeTarget target = new BridgeTarget("overlay");
        check("the target works before anything happened", target.work(3, "abcd"), 34);

        failures += run(BridgeTarget.class, args[1]);

        System.out.println("\n-- after the rewrite --");
        check("the original body still returns its value", target.work(3, "abcd"), 34);
        check("the bridge ran", Class.forName("HookBridge")
                                     .getField("enters").getInt(null), 1);

        System.out.println("\n-- what native was handed --");
        check("it was called once", reports(), 1);
        check("identifier", reportedId(), 7);
        // scale(3) = 30 in Java, plus "abcd".length() = 4
        check("a value the bridge computed in Java", reportedValue(), 34);
        check("a label the bridge read off the receiver", reportedLabel(), "overlay");

        shutdown();

        System.out.println(failures == 0
            ? "\nthe object work happened in Java; native only saw an int and a string"
            : "\n" + failures + " CHECKS FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
