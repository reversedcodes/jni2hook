import java.lang.reflect.Method;
import java.nio.file.Files;
import java.nio.file.Path;

/* Compares a rewritten class against the untouched one. Calling a method forces
   the JVM to link and verify the class, so a wrong offset in the exception
   table or a stale frame in the StackMapTable shows up as a VerifyError right
   here rather than as a puzzling crash later. */
public class MidHookTest {

    static native int bind(Class<?> target, String name);
    static native int midCalls();
    static native void resetMid();

    static final class Loader extends ClassLoader {
        Class<?> define(String name, byte[] bytes) {
            return defineClass(name, bytes, 0, bytes.length);
        }
    }

    static int failures = 0;

    static void check(String what, Object actual, Object expected) {
        boolean ok = expected.equals(actual);
        System.out.printf("  %-56s %s%n", what + " = " + actual,
                          ok ? "ok" : "FAIL expected " + expected);
        if (!ok) failures++;
    }

    static void check(String what, boolean condition) {
        System.out.printf("  %-56s %s%n", what, condition ? "ok" : "FAIL");
        if (!condition) failures++;
    }

    static Object run(Class<?> c, int n) throws Exception {
        Object instance = c.getDeclaredConstructor().newInstance();
        Method m = c.getDeclaredMethod("compute", int.class);
        return m.invoke(instance, n);
    }

    public static void main(String[] args) throws Exception {
        System.load(args[0]);

        byte[] original = Files.readAllBytes(Path.of(args[1]));
        Class<?> plain = new Loader().define("MidHookTarget", original);
        Object expected = run(plain, 20);
        System.out.println("original compute(20) = " + expected);

        for (int i = 2; i + 1 < args.length; i += 2) {
            String file = args[i];
            String hookName = args[i + 1];
            System.out.println("\n" + Path.of(file).getFileName() + "  hook " + hookName);

            byte[] rewritten = Files.readAllBytes(Path.of(file));
            Class<?> c;
            try {
                c = new Loader().define("MidHookTarget", rewritten);
            } catch (Throwable t) {
                check("class loads: " + t, false);
                continue;
            }

            check("native binding registered", bind(c, hookName) == 0);
            resetMid();

            Object actual;
            try {
                actual = run(c, 20);
            } catch (Throwable t) {
                Throwable cause = t.getCause() != null ? t.getCause() : t;
                check("compute(20) runs: " + cause, false);
                continue;
            }

            check("compute(20) still returns the original value", actual, expected);
            check("the hook actually fired", midCalls() > 0);
            System.out.println("    hook fired " + midCalls() + " times");
        }

        System.out.println(failures == 0 ? "\nall midhook checks passed" : "\n" + failures + " CHECKS FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
