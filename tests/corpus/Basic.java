import java.io.Serializable;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

@Deprecated
public class Basic<T extends Comparable<T>> implements Serializable {

    public @interface Marker { String value() default "x"; }

    public enum Color { RED, GREEN, BLUE }

    public interface Callback { void call(String message); }

    public static class Inner {
        private final long bits = 0x1234_5678_9ABC_DEF0L;
        long bits() { return bits; }
    }

    private static final long   BIG    = 9_223_372_036_854_775_807L;
    private static final double PI     = 3.141592653589793d;
    private static final float  E      = 2.71828f;
    private static final int    COUNT  = 42;
    private static final String NAME   = "jni2hookäöü";

    private T value;
    private final List<Map<String, int[]>> nested = new ArrayList<Map<String, int[]>>();

    public Basic() { }

    public Basic(T value) { this.value = value; }

    static { System.out.println(NAME + BIG + PI + E + COUNT); }

    @Marker("annotated")
    public synchronized T get() throws IllegalStateException {
        if (value == null) throw new IllegalStateException(NAME);
        return value;
    }

    public native int nativeMethod(long handle, Object[] args);

    public strictfp double compute(double a, double b) {
        double total = 0.0d;
        for (int i = 0; i < COUNT; i++) {
            switch (i % 3) {
                case 0:  total += a * PI; break;
                case 1:  total -= b / PI; break;
                default: total += Math.sqrt(Math.abs(a - b)); break;
            }
        }
        return total;
    }

    public void resources(Callback callback) {
        try {
            callback.call(NAME);
        } catch (RuntimeException e) {
            throw new IllegalStateException(e);
        } finally {
            nested.clear();
        }
    }

    public String describe(Color color) {
        if (color == Color.RED)   return "red";
        if (color == Color.GREEN) return "green";
        return String.valueOf(color);
    }
}
