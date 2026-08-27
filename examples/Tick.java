public class Tick {

    int ticks;

    void tick() {
        ticks++;
    }

    public static void main(String[] args) {
        System.load(args[0]);

        Tick game = new Tick();
        for (int i = 0; i < 3; i++) {
            game.tick();
        }
        System.out.println("ticks = " + game.ticks);
    }
}
