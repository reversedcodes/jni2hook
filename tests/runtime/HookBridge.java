/* The piece that is meant to make JNI unnecessary for the boring half of a hook.
 *
 * It is compiled by javac, kept out of the application's class path, and handed
 * to DefineClass at run time as a byte buffer -- into the *target's own* class
 * loader, not the bootstrap one. That is the whole trick: a class defined
 * alongside the target can name the target's types, so everything it does with
 * them is plain Java. No CallObjectMethod, no descriptor strings, no
 * ExceptionCheck after every step, no local references to release.
 *
 * Native is then only called for what actually wants to be native, and it
 * receives primitives and strings rather than a graph of jobjects to walk. */
public final class HookBridge {

    /* Bound with RegisterNatives on this class, which is allowed because this
       class was defined rather than redefined. */
    public static native void report(int id, int value, String label);

    public static int enters = 0;

    /* The signature the inserted call site produces: the identifier first, then
       the receiver and every argument, references widened to Object because the
       call site cannot name types the callee's loader might not have. */
    public static void enter(int id, Object receiver, int number, Object text) {
        enters++;

        BridgeTarget target = (BridgeTarget) receiver;

        // Ordinary Java against the hooked class. This is the part that would
        // otherwise be a stack of JNI calls.
        int value = target.scale(number) + String.valueOf(text).length();

        report(id, value, target.label());
    }
}
