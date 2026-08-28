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
}
