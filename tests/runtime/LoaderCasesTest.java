import java.lang.reflect.Method;
import java.nio.file.Files;
import java.nio.file.Path;

/* The bridge works when the target sits on the application class path in the
   unnamed module. Nothing in a real game does.
 *
 * Two things about the arrangement could break it, and neither has anything to
 * do with which mod loader is in use:
 *
 *   loader   the target belongs to a class loader of its own. DefineClass takes
 *            a loader, so this should hold, but it had not been tried.
 *
 *   module   the target sits in a *named* module. A class defined through
 *            DefineClass lands in its loader's unnamed module, and a named
 *            module does not read the unnamed one, so the inserted invokestatic
 *            crosses a boundary the verifier is entitled to refuse.
 *
 * Which case runs is decided by how this is started: with the target on the
 * class path it is loaded here by hand into a loader of its own, and with the
 * target on the module path it is already in a named module and simply looked
 * up. */
public class LoaderCasesTest {

    static native int install(Class<?> target, String bridgeClassFile);
    static native int reports();
    static native int reportedValue();
    static native void shutdown();

    static final class Isolated extends ClassLoader {
        Isolated(ClassLoader parent) {
            super("isolated", parent);
        }

        Class<?> define(String name, byte[] bytes) {
            return defineClass(name, bytes, 0, bytes.length);
        }
    }

    static int failures = 0;

    static void check(String what, boolean condition) {
        System.out.printf("  %-58s %s%n", what, condition ? "ok" : "FAIL");
        if (!condition) failures++;
    }

    static void check(String what, Object actual, Object expected) {
        boolean ok = expected.equals(actual);
        System.out.printf("  %-44s = %-16s %s%n", what, actual,
                          ok ? "ok" : "FAIL expected " + expected);
        if (!ok) failures++;
    }

    public static void main(String[] args) throws Exception {
        System.load(args[0]);

        String mode = args[1];
        String bridge = args[2];

        Class<?> target;
        if (mode.equals("loader")) {
            byte[] bytes = Files.readAllBytes(Path.of(args[3]));
            Isolated loader = new Isolated(LoaderCasesTest.class.getClassLoader());
            target = loader.define("BridgeTarget", bytes);
            System.out.println("  target loader: " + target.getClassLoader().getName());
            System.out.println("  target module: " + target.getModule());
        } else {
            target = Class.forName(args[3]);
            System.out.println("  target loader: " + target.getClassLoader().getName());
            System.out.println("  target module: " + target.getModule());
            check("the target really is in a named module", target.getModule().isNamed());
        }

        Object instance = target.getConstructor(String.class).newInstance("scoped");
        Method work = target.getMethod("work", int.class, String.class);
        check("the target works before anything happened", work.invoke(instance, 3, "abcd"), 34);

        failures += install(target, bridge);

        boolean ran;
        try {
            check("the original body still returns its value",
                  work.invoke(instance, 3, "abcd"), 34);
            ran = reports() == 1;
        } catch (Throwable failure) {
            Throwable cause = failure.getCause() != null ? failure.getCause() : failure;
            System.out.println("  calling the rewritten method threw: " + cause);
            ran = false;
            failures++;
        }

        check("the bridge was reached", ran);
        if (ran) {
            check("it called through the bound handle", reportedValue(), 34);
        }

        shutdown();

        System.out.println(failures == 0 ? "\n" + mode + ": the bridge holds"
                                         : "\n" + mode + ": " + failures + " CHECKS FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
