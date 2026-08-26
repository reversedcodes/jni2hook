public class HookTarget implements java.util.function.Supplier<String> {

    private final int seed;

    public HookTarget(int seed) { this.seed = seed; }

    public int compute(int a, int b) {
        int total = seed;
        for (int i = 0; i < a; i++) {
            total += b * i;
        }
        return total;
    }

    public static String greet(String who) {
        if (who == null) {
            throw new IllegalArgumentException("who");
        }
        return "hello " + who.toUpperCase();
    }

    public synchronized int locked(int x) {
        return x * 2;
    }

    @Override
    public String get() {
        return "supplied";
    }

    public long mixed(long a, double b, Object c) {
        try {
            return a + (long) b + (c == null ? 0 : c.hashCode());
        } catch (RuntimeException e) {
            return -1L;
        }
    }
}
