import java.lang.reflect.Field;
import java.lang.reflect.Method;

/* A hook that decides whether the original body runs at all.

   Everything so far has been observation: the inserted call runs and the body
   runs after it. This is the other half -- skipping the body and returning in
   its place -- which needs a branch, and a branch target needs a StackMapTable
   frame. The frame is only derivable at method entry, so that is the one place
   this works, and the JVM's verifier is what decides whether the derivation was
   right. */
public class GuardTest {

    static native int installGuard(Class<?> target, String bridgeClassFile);
    static native int reports();
    static native int reportedValue();
    static native void shutdown();

    static int failures = 0;

    static void check(String what, Object actual, Object expected) {
        boolean ok = expected == null ? actual == null : expected.equals(actual);
        System.out.printf("  %-46s = %-14s %s%n", what, actual,
                          ok ? "ok" : "FAIL expected " + expected);
        if (!ok) failures++;
    }

    static void check(String what, boolean condition) {
        System.out.printf("  %-60s %s%n", what, condition ? "ok" : "FAIL");
        if (!condition) failures++;
    }

    public static void main(String[] args) throws Exception {
        System.load(args[0]);

        BridgeTarget target = new BridgeTarget("guarded");
        check("the target works before anything happened", target.work(3, "abcd"), 34);

        failures += installGuard(BridgeTarget.class, args[1]);

        Class<?> bridge = Class.forName("HandleBridge");
        Field cancel = bridge.getField("cancel");
        Field guardCalls = bridge.getField("guardCalls");

        System.out.println("\n-- guard says no, the body runs --");
        cancel.setBoolean(null, false);
        int value;
        try {
            value = target.work(3, "abcd");
        } catch (VerifyError error) {
            System.out.println("  the rewritten method did not verify: " + error.getMessage());
            System.exit(1);
            return;
        }
        check("the original value comes back", value, 34);
        check("the guard ran", guardCalls.getInt(null), 1);
        check("and did its own work on the way", reportedValue(), 34);

        System.out.println("\n-- guard says yes, the body is skipped --");
        cancel.setBoolean(null, true);
        check("the method returns its type's default", target.work(3, "abcd"), 0);
        check("the guard ran again", guardCalls.getInt(null), 2);

        System.out.println("\n-- and back again, so it is a decision and not a state --");
        cancel.setBoolean(null, false);
        check("the body runs once more", target.work(5, "xy"), 52);

        System.out.println("\n-- the shapes the frame builder has to get right --");
        String[] items = {"first", "last"};

        BridgeTarget.acted = 0;
        target.act(4L, 2.5);
        check("void with a long and a double: the body ran", BridgeTarget.acted, 6);
        check("an array parameter and a reference return", target.pick(items, true), "first");
        check("a static method with no receiver", BridgeTarget.stat(4), 12);

        cancel.setBoolean(null, true);
        BridgeTarget.acted = 0;
        target.act(4L, 2.5);
        check("void skipped: nothing happened", BridgeTarget.acted, 0);
        check("reference skipped: null comes back", target.pick(items, true), null);
        check("static skipped: zero comes back", BridgeTarget.stat(4), 0);
        cancel.setBoolean(null, false);

        shutdown();

        System.out.println(failures == 0 ? "\na hook can skip the body it replaces"
                                         : "\n" + failures + " CHECKS FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
