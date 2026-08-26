/* Drives jni2hook against a live JVM: the library is loaded into a running VM
   the way an injected library would be, so the JVMTI environment is created in
   the live phase and only gets the capabilities that phase still grants. */
public class HookRuntimeTest {

    static native int setup();
    static native int hookCompute(Class<?> target);
    static native int hookGreet(Class<?> target);
    static native int hookLocked(Class<?> target);
    static native int unhookCompute(Class<?> target);
    static native int unhookGreet(Class<?> target);
    static native int unhookLocked(Class<?> target);
    static native int calls();
    static native void resetCalls();
    static native void teardown();

    public int compute(int a, int b) {
        return a + b;
    }

    public static String greet(String who) {
        return "hello " + who;
    }

    public synchronized int locked(int x) {
        return x * 2;
    }

    static int failures = 0;

    static void check(String what, boolean condition) {
        System.out.printf("  %-58s %s%n", what, condition ? "ok" : "FAIL");
        if (!condition) failures++;
    }

    static void check(String what, Object actual, Object expected) {
        boolean ok = expected.equals(actual);
        System.out.printf("  %-58s %s%n", what + " = " + actual, ok ? "ok" : "FAIL expected " + expected);
        if (!ok) failures++;
    }

    public static void main(String[] args) throws Exception {
        System.load(args[0]);

        HookRuntimeTest o = new HookRuntimeTest();

        check("before any hook: compute(2,3)", o.compute(2, 3), 5);
        check("before any hook: greet", greet("world"), "hello world");
        check("before any hook: locked(21)", o.locked(21), 42);

        check("JNI2Hook_Init", setup() == 0);

        check("install compute", hookCompute(HookRuntimeTest.class) == 0);
        resetCalls();
        check("hooked compute(2,3) goes through the detour", o.compute(2, 3), 50);
        check("detour ran exactly once", calls(), 1);
        check("greet untouched while compute is hooked", greet("world"), "hello world");

        check("install greet on the same class", hookGreet(HookRuntimeTest.class) == 0);
        resetCalls();
        check("hooked greet", greet("world"), "hooked:hello world");
        check("compute still hooked after a second install", o.compute(2, 3), 50);
        check("both detours ran", calls(), 2);

        check("install locked (synchronized)", hookLocked(HookRuntimeTest.class) == 0);
        resetCalls();
        check("hooked locked(21) keeps working", o.locked(21), 43);
        check("locked detour ran", calls(), 1);

        check("uninstall greet only", unhookGreet(HookRuntimeTest.class) == 0);
        check("greet back to original", greet("world"), "hello world");
        check("compute survives the unrelated uninstall", o.compute(2, 3), 50);
        check("locked survives the unrelated uninstall", o.locked(21), 43);

        check("uninstall compute", unhookCompute(HookRuntimeTest.class) == 0);
        check("compute back to original", o.compute(2, 3), 5);
        check("locked still hooked", o.locked(21), 43);

        check("uninstall locked", unhookLocked(HookRuntimeTest.class) == 0);
        check("locked back to original", o.locked(21), 42);

        check("reinstall after full uninstall", hookCompute(HookRuntimeTest.class) == 0);
        check("compute hooked again", o.compute(2, 3), 50);

        teardown();
        check("after shutdown everything is original: compute", o.compute(2, 3), 5);
        check("after shutdown everything is original: greet", greet("world"), "hello world");
        check("after shutdown everything is original: locked", o.locked(21), 42);

        System.out.println(failures == 0 ? "all runtime checks passed" : failures + " CHECKS FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
