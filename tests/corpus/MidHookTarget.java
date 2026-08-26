public class MidHookTarget {

    public static int hookCalls = 0;

    public int compute(int n) {
        int total = 0;
        for (int i = 0; i < n; i++) {
            switch (i % 4) {
                case 0:  total += i; break;
                case 1:  total -= i; break;
                case 2:  total *= 2; break;
                default: total += 7; break;
            }
            try {
                if (i == 13) {
                    throw new IllegalStateException("thirteen");
                }
                total += 1;
            } catch (IllegalStateException e) {
                total += 100;
            } finally {
                total += 2;
            }
        }
        return total;
    }

    public static String describe(long value) {
        StringBuilder sb = new StringBuilder();
        for (long v = value; v > 0; v /= 10) {
            sb.append((char) ('0' + (v % 10)));
        }
        return sb.reverse().toString();
    }
}
