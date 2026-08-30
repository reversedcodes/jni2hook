public class SimpleTarget {
    void hello() {
        System.out.println("hello from Java");
    }

    int add(int left, int right) {
        return left + right;
    }

    static int multiply(int left, int right) {
        return left * right;
    }

    public static void main(String[] args) throws Exception {
        System.load(args[0]);

        // The native loader entry schedules installation on a worker thread.
        Thread.sleep(500);

        SimpleTarget target = new SimpleTarget();
        target.hello();
        System.out.println("add = " + target.add(2, 3));
        System.out.println("multiply = " + multiply(4, 5));
    }
}
