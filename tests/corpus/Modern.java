import java.util.List;
import java.util.function.Function;
import java.util.stream.Collectors;

public class Modern {

    public record Point(int x, int y) { }

    public sealed interface Shape permits Circle, Square { }
    public record Circle(double radius) implements Shape { }
    public record Square(double side)   implements Shape { }

    private static final String BLOCK = """
            a text block
            spanning lines
            """;

    public List<String> lambdas(List<Point> points) {
        Function<Point, String> render = p -> p.x() + ":" + p.y();
        return points.stream()
                     .filter(p -> p.x() > 0)
                     .map(render)
                     .sorted()
                     .collect(Collectors.toList());
    }

    public double area(Shape shape) {
        return switch (shape) {
            case Circle c -> Math.PI * c.radius() * c.radius();
            case Square s -> s.side() * s.side();
        };
    }

    public String block() { return BLOCK; }

    public String dispatch(Object o) {
        if (o instanceof Point p && p.x() > 0) return "point " + p;
        return switch (o) {
            case Integer i when i > 10 -> "big int";
            case String s              -> "string " + s.length();
            case null                  -> "null";
            default                    -> "other";
        };
    }
}
