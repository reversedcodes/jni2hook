import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.stream.Stream;

/* Hands every class file below a directory to defineClass, which runs HotSpot's
   own class file parser and format checker over it. A ClassFormatError means the
   rewrite produced something the JVM rejects; missing dependencies are counted
   separately because they say nothing about the rewrite. */
public class DefineAll {

    static final class Loader extends ClassLoader {
        Class<?> define(byte[] bytes) {
            return defineClass(null, bytes, 0, bytes.length);
        }
    }

    public static void main(String[] args) throws IOException {
        List<Path> files = new ArrayList<>();
        try (Stream<Path> walk = Files.walk(Path.of(args[0]))) {
            walk.filter(p -> p.toString().endsWith(".class")).forEach(files::add);
        }

        int accepted = 0, unresolved = 0, rejected = 0;
        List<String> firstErrors = new ArrayList<>();

        for (Path file : files) {
            byte[] bytes = Files.readAllBytes(file);
            try {
                new Loader().define(bytes);
                accepted++;
            } catch (ClassFormatError | VerifyError e) {
                rejected++;
                if (firstErrors.size() < 10) {
                    firstErrors.add(file.getFileName() + ": " + e.getClass().getSimpleName()
                                    + ": " + e.getMessage());
                }
            } catch (LinkageError | SecurityException e) {
                unresolved++;
            }
        }

        firstErrors.forEach(e -> System.out.println("  " + e));
        System.out.printf("  %d accepted by the JVM, %d rejected, %d skipped (missing deps)%n",
                          accepted, rejected, unresolved);
        System.exit(rejected == 0 ? 0 : 1);
    }
}
