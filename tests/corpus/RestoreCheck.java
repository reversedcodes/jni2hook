import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.nio.file.Files;
import java.nio.file.Path;

/* Checks that undoing the rewrite gives back a class that behaves like the one
   we started from: no native methods, no copies, original bodies callable. */
public class RestoreCheck {

    static final class Loader extends ClassLoader {
        Class<?> define(String name, byte[] bytes) {
            return defineClass(name, bytes, 0, bytes.length);
        }
    }

    static int failures = 0;

    static void check(String what, boolean condition) {
        System.out.printf("  %-58s %s%n", what, condition ? "ok" : "FAIL");
        if (!condition) failures++;
    }

    public static void main(String[] args) throws Exception {
        byte[] bytes = Files.readAllBytes(Path.of(args[0]));
        Class<?> c = new Loader().define("HookTarget", bytes);
        System.out.println("restored class loaded and verified: " + c.getName());

        for (Method m : c.getDeclaredMethods()) {
            check("no native left: " + m.getName(), !Modifier.isNative(m.getModifiers()));
            check("no copy left: " + m.getName(), !m.getName().contains("$jni2hook"));
        }

        Object instance = c.getDeclaredConstructor(int.class).newInstance(100);

        Method compute = c.getDeclaredMethod("compute", int.class, int.class);
        check("compute runs directly again (expect 130): " + compute.invoke(instance, 5, 3),
              compute.invoke(instance, 5, 3).equals(130));

        Method greet = c.getDeclaredMethod("greet", String.class);
        check("greet runs directly again: " + greet.invoke(null, "world"),
              "hello WORLD".equals(greet.invoke(null, "world")));

        Method mixed = c.getDeclaredMethod("mixed", long.class, double.class, Object.class);
        check("mixed runs directly again (expect 42): " + mixed.invoke(instance, 40L, 2.9d, null),
              mixed.invoke(instance, 40L, 2.9d, null).equals(42L));

        System.out.println(failures == 0 ? "all checks passed" : failures + " CHECKS FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
