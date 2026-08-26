import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.nio.file.Files;
import java.nio.file.Path;

/* Loads a transformed HookTarget through a throwaway loader, which runs the JVM
   verifier over it, then checks that the original became native and that the
   copy still carries a working body. */
public class HookCheck {

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
        System.out.println("class loaded and verified by the JVM: " + c.getName());

        Method compute = c.getDeclaredMethod("compute", int.class, int.class);
        check("compute(II)I is native", Modifier.isNative(compute.getModifiers()));
        check("compute(II)I is still public", Modifier.isPublic(compute.getModifiers()));

        Method copy = c.getDeclaredMethod("compute$jni2hook", int.class, int.class);
        check("copy is private", Modifier.isPrivate(copy.getModifiers()));
        check("copy is final", Modifier.isFinal(copy.getModifiers()));
        check("copy is not native", !Modifier.isNative(copy.getModifiers()));

        Object instance = c.getDeclaredConstructor(int.class).newInstance(100);
        copy.setAccessible(true);
        Object value = copy.invoke(instance, 5, 3);
        check("copy still computes the original body (expect 130): " + value,
              value.equals(130));

        try {
            compute.invoke(instance, 5, 3);
            check("calling the native method without a binding throws", false);
        } catch (Exception e) {
            Throwable cause = e.getCause() != null ? e.getCause() : e;
            check("unbound native method throws UnsatisfiedLinkError",
                  cause instanceof UnsatisfiedLinkError);
        }

        Method greet = c.getDeclaredMethod("greet", String.class);
        check("greet is native", Modifier.isNative(greet.getModifiers()));
        Method greetCopy = c.getDeclaredMethod("greet$jni2hook", String.class);
        check("static copy is static", Modifier.isStatic(greetCopy.getModifiers()));
        greetCopy.setAccessible(true);
        Object greeting = greetCopy.invoke(null, "world");
        check("static copy still runs (expect 'hello WORLD'): " + greeting,
              "hello WORLD".equals(greeting));

        Method mixed = c.getDeclaredMethod("mixed", long.class, double.class, Object.class);
        check("mixed is native", Modifier.isNative(mixed.getModifiers()));
        Method mixedCopy = c.getDeclaredMethod("mixed$jni2hook", long.class, double.class, Object.class);
        mixedCopy.setAccessible(true);
        Object mixedValue = mixedCopy.invoke(instance, 40L, 2.9d, null);
        check("copy with long/double/Object args works (expect 42): " + mixedValue,
              mixedValue.equals(42L));

        Method lockedCopy = c.getDeclaredMethod("locked$jni2hook", int.class);
        check("copy of a synchronized method keeps ACC_SYNCHRONIZED",
              Modifier.isSynchronized(lockedCopy.getModifiers()));
        lockedCopy.setAccessible(true);
        check("synchronized copy runs (expect 84): " + lockedCopy.invoke(instance, 42),
              lockedCopy.invoke(instance, 42).equals(84));

        Method bridgeCopy = null;
        for (Method m : c.getDeclaredMethods()) {
            if (m.getName().equals("get$jni2hook") && m.getReturnType() == Object.class) {
                bridgeCopy = m;
            }
        }
        check("copy of the covariant bridge exists", bridgeCopy != null);
        if (bridgeCopy != null) {
            check("bridge copy keeps ACC_BRIDGE", bridgeCopy.isBridge());
            check("bridge copy keeps ACC_SYNTHETIC", bridgeCopy.isSynthetic());
        }

        System.out.println(failures == 0 ? "all checks passed" : failures + " CHECKS FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
