package ens.lang;

import java.io.File;
import java.io.InputStreamReader;
import java.io.Reader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.HashMap;
import java.util.Map;

public class Main {

    public static void main(String[] args) {
        Map<String, String> arguments = parseArguments(args);

        if (arguments.containsKey("-h") || arguments.containsKey("-help")) {
            System.out.println("Use -source to specify the input file or folder to compile, otherwise use stdin");
            System.out.println("Use -output to specify the output folder");
            System.exit(0);
        }


        if (execute(arguments)) {
            String output = arguments.get("output");
            System.out.println("Compiled successfully to " + (output == null ? "the current folder" : output));
        } else {
            System.err.println("Please check the errors and retry.");
        }
    }

    private static boolean execute(Map<String, String> arguments) {
        File outputFolder = new File(arguments.getOrDefault("output", "."));
        if (outputFolder.exists() && !outputFolder.isDirectory()) {
            throw new IllegalArgumentException("The specified output directory is not a directory, please choose a different folder");
        }

        if (arguments.containsKey("source")) {
            File source = new File(arguments.get("source"));
            Path sourcePath = source.isDirectory() ? source.toPath() : source.getParentFile().toPath();
            return Compiler.compile(source, outputFolder, sourcePath);
        } else {
            Reader source = new InputStreamReader(System.in, StandardCharsets.UTF_8);
            return Compiler.compileSingle(source, outputFolder);
        }

    }

    private static Map<String, String> parseArguments(String[] args) {
        Map<String, String> arguments = new HashMap<>();

        for (int i = 0; i < args.length; i++) {
            if (args[i].charAt(0) == '-') {
                if (args[i].length() < 2) {
                    throw new IllegalArgumentException("Not a valid argument: " + args[i]);
                }
                if (args.length - 1 == i) {
                    throw new IllegalArgumentException("Expected arg after: " + args[i]);
                }
                arguments.put(args[i].substring(1), args[i + 1]);
                i++;
            }
        }
        return arguments;
    }
}
