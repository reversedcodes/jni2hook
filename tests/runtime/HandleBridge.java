import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.reflect.Method;

/* The same bridge as HookBridge, with the one thing that made it fragile taken
   out: it does not name the class it works on, anywhere.
 *
 * HookBridge had to write (BridgeTarget) receiver, and the Minecraft equivalent
 * of that name is class_310, which is renamed every version. Here the receiver
 * stays Object -- the Java equivalent of a void* -- and the methods to call on
 * it arrive as MethodHandles that the native side bound beforehand.
 *
 * Nothing in this file would have to change if every type it touches were
 * renamed. It compiles without the target class being present at all. */
public final class HandleBridge {

    public static native void report(int id, int value);

    private static final MethodHandle[] BOUND = new MethodHandle[8];

    public static int enters = 0;

    /* Called from native with a java.lang.reflect.Method it resolved by class
       file slot, so no name was written down on either side. */
    public static void bind(int slot, Object reflected) throws Throwable {
        Method method = (Method) reflected;
        method.setAccessible(true);
        BOUND[slot] = MethodHandles.lookup().unreflect(method);
    }

    /* Guard for a replacing hook: true means the original body is skipped. */
    public static volatile boolean cancel = false;

    /* When set, the hook calls the original itself, does something with the
       result and returns that instead. This is the piece that used to need the
       body parked in a copy, and with it the VM flag that allows adding a
       method during redefinition. */
    public static volatile boolean intercept = false;

    public static int guardCalls = 0;
    public static int originalCalls = 0;

    /* Calling the original means calling the method that is hooked, so the
       guard runs again on the way in. The depth marker is what makes the inner
       call fall through to the real body instead of recursing. */
    private static final ThreadLocal<Boolean> INSIDE =
            ThreadLocal.withInitial(() -> Boolean.FALSE);
    private static final ThreadLocal<Integer> VALUE = ThreadLocal.withInitial(() -> 0);

    public static boolean guard(int id, Object receiver, int number, Object text)
            throws Throwable {
        if (INSIDE.get()) {
            originalCalls++;
            return false;
        }

        guardCalls++;

        if (intercept) {
            INSIDE.set(Boolean.TRUE);
            int original;
            try {
                original = (int) BOUND[1].invoke(receiver, number, text);
            } finally {
                INSIDE.set(Boolean.FALSE);
            }
            VALUE.set(original * 100);
            return true;
        }

        // Still ordinary Java against an object it cannot name.
        int scaled = (int) BOUND[0].invoke(receiver, number);
        report(id, scaled + String.valueOf(text).length());
        return cancel;
    }

    public static int guardValue(int id) {
        return VALUE.get();
    }

    /* Guards for the other shapes. They only have to answer the question; the
       inserted bytecode supplies the return value. */
    public static boolean guardVoid(int id, Object receiver, long big, double fraction) {
        guardCalls++;
        return cancel;
    }

    public static boolean guardRef(int id, Object receiver, Object items, boolean flag) {
        guardCalls++;
        return cancel;
    }

    public static boolean guardStatic(int id, int value) {
        guardCalls++;
        return cancel;
    }

    public static void enter(int id, Object receiver, int number, Object text) throws Throwable {
        enters++;

        /* invoke adapts the handle's real signature to the Object it is given,
           so the receiver never needs a static type here. */
        int scaled = (int) BOUND[0].invoke(receiver, number);

        report(id, scaled + String.valueOf(text).length());
    }
}
