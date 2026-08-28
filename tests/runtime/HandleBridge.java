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

    public static void enter(int id, Object receiver, int number, Object text) throws Throwable {
        enters++;

        /* invoke adapts the handle's real signature to the Object it is given,
           so the receiver never needs a static type here. */
        int scaled = (int) BOUND[0].invoke(receiver, number);

        report(id, scaled + String.valueOf(text).length());
    }
}
