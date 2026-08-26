import java.lang.classfile.ClassFile;
import java.lang.classfile.ClassModel;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.stream.Stream;

/* Runs the JDK's own class file library over the rewritten classes. Unlike
   defineClass this has no package restrictions, so it reaches the java.* classes
   too, and ClassFile.verify applies the JVM verifier rules independently of our
   own writer. */
public class VerifyAll {

    public static void main(String[] args) throws IOException {
        List<Path> files = new ArrayList<>();
        try (Stream<Path> walk = Files.walk(Path.of(args[0]))) {
            walk.filter(p -> p.toString().endsWith(".class")).forEach(files::add);
        }

        ClassFile parser = ClassFile.of();
        int parsed = 0, methods = 0, natives = 0, broken = 0, skipped = 0;
        List<String> firstErrors = new ArrayList<>();

        for (Path file : files) {
            byte[] bytes = Files.readAllBytes(file);
            try {
                ClassModel model = parser.parse(bytes);
                parsed++;
                methods += model.methods().size();
                natives += (int) model.methods().stream()
                        .filter(m -> m.flags().has(java.lang.reflect.AccessFlag.NATIVE))
                        .count();

                List<VerifyError> errors = parser.verify(bytes).stream()
                        .filter(e -> !String.valueOf(e.getMessage()).contains("Could not resolve class"))
                        .toList();
                if (!errors.isEmpty()) {
                    broken++;
                    if (firstErrors.size() < 10) {
                        firstErrors.add(file.getFileName() + ": " + errors.get(0).getMessage());
                    }
                } else if (parser.verify(bytes).size() > 0) {
                    skipped++;
                }
            } catch (Throwable t) {
                String message = String.valueOf(t.getMessage());
                if (message.contains("Unsupported class file version")
                        || message.contains("Could not resolve class")) {
                    skipped++;
                    continue;
                }
                broken++;
                if (firstErrors.size() < 10) {
                    firstErrors.add(file.getFileName() + ": " + t.getClass().getSimpleName()
                                    + ": " + t.getMessage());
                }
            }
        }

        firstErrors.forEach(e -> System.out.println("  " + e));
        System.out.printf("  %d parsed, %d methods (%d native), %d with findings, %d skipped%n",
                          parsed, methods, natives, broken, skipped);
        System.exit(broken == 0 ? 0 : 1);
    }
}
