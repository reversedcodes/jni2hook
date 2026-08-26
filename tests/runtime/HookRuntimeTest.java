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
    static native int hookInsertFirst(Class<?> target);
    static native int hookInsertSecond(Class<?> target);
    static native int hookInsertStatic(Class<?> target);
    static native int unhookInsert(Class<?> target);
    static native int unhookInsertStatic(Class<?> target);
    static native int calls();
    static native void resetCalls();
    static native int insertFirstCalls();
    static native int insertSecondCalls();
    static native int insertStaticCalls();
    static native void resetInsertCalls();
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

    public int insertTarget() {
        int value = 1;
        return value;
    }

    public static int staticInsertTarget() {
        return 9;
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

        check("install call at original offset 0", hookInsertFirst(HookRuntimeTest.class) == 0);
        resetInsertCalls();
        check("body continues after inserted call", o.insertTarget(), 1);
        check("first inserted call ran", insertFirstCalls(), 1);
        check("second inserted call not installed yet", insertSecondCalls(), 0);

        check("install call at original offset 2", hookInsertSecond(HookRuntimeTest.class) == 0);
        resetInsertCalls();
        check("body survives two inserted calls", o.insertTarget(), 1);
        check("first inserted call survives reapply", insertFirstCalls(), 1);
        check("second inserted call ran", insertSecondCalls(), 1);

        check("install replacement beside inserted calls", hookCompute(HookRuntimeTest.class) == 0);
        resetCalls();
        resetInsertCalls();
        check("replacement works beside inserted calls", o.compute(2, 3), 50);
        check("inserted calls work beside replacement", o.insertTarget(), 1);
        check("replacement detour ran beside inserted calls", calls(), 1);
        check("both inserted calls ran beside replacement",
                insertFirstCalls() == 1 && insertSecondCalls() == 1);
        check("uninstall replacement beside inserted calls", unhookCompute(HookRuntimeTest.class) == 0);
        resetInsertCalls();
        check("inserted calls survive replacement uninstall", o.insertTarget(), 1);
        check("both inserted calls survived replacement uninstall",
                insertFirstCalls() == 1 && insertSecondCalls() == 1);

        check("install static inserted call", hookInsertStatic(HookRuntimeTest.class) == 0);
        resetInsertCalls();
        check("static body continues after inserted call", staticInsertTarget(), 9);
        check("static inserted call ran", insertStaticCalls(), 1);

        check("uninstall removes both calls for the method", unhookInsert(HookRuntimeTest.class) == 0);
        resetInsertCalls();
        check("instance body remains original after uninstall", o.insertTarget(), 1);
        check("first inserted call removed", insertFirstCalls(), 0);
        check("second inserted call removed", insertSecondCalls(), 0);
        check("static inserted call survives unrelated uninstall", staticInsertTarget(), 9);
        check("static inserted call still ran", insertStaticCalls(), 1);

        check("uninstall static inserted call", unhookInsertStatic(HookRuntimeTest.class) == 0);
        resetInsertCalls();
        check("static body remains original after uninstall", staticInsertTarget(), 9);
        check("static inserted call removed", insertStaticCalls(), 0);

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
