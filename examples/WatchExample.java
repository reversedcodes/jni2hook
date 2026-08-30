public class WatchExample {
    private static native void installPreparedHook();

    static final class LateTarget {
        int compute(int value) {
            return value * 3 + 7;
        }
    }

    public static void main(String[] args) throws Exception {
        System.load(args[0]);

        // Give the native loader thread time to register before LateTarget loads.
        Thread.sleep(500);

        LateTarget target = new LateTarget();
        installPreparedHook();
        System.out.println("compute = " + target.compute(5));
    }
}
