/* Stands in for a hooked game class: it has a method worth hooking and other
   methods the bridge will want to call on it. */
public class BridgeTarget {

    private final String label;

    public BridgeTarget(String label) {
        this.label = label;
    }

    public String label() {
        return label;
    }

    public int scale(int value) {
        return value * 10;
    }

    public int work(int number, String text) {
        return scale(number) + text.length();
    }

    public static int acted = 0;

    /* The shapes that make a hand-built stack map frame interesting: a long and
       a double take two local slots but one frame entry each, an array is a
       reference whose verification type names the descriptor rather than a
       class, a void method has nothing to return, and a static one has no
       receiver in slot zero. */
    public void act(long big, double fraction) {
        acted += (int) big + (int) fraction;
    }

    public String pick(String[] items, boolean flag) {
        return flag ? items[0] : items[items.length - 1];
    }

    public static int stat(int value) {
        return value * 3;
    }
}
