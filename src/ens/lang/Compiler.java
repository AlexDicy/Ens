package ens.lang;

import ens.lang.tokenizer.Token;
import ens.lang.tokenizer.Tokenizer;

import java.io.*;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.stream.Collectors;

public class Compiler {
    private static final FileFilter ensFileFilter = file -> file.isDirectory() || (file.isFile() && file.getName().endsWith(".ens"));

    public static boolean compile(File source, File outputFolder, Path sourcePath) {
        if (source.isDirectory()) {
            List<File> files = getFileTree(source, sourcePath);
            for (File file : files) {
                compileSingle(source, file, outputFolder);
            }
        } else {
            return compileSingle(null, source, outputFolder);
        }
        return true;
    }

    private static List<File> getFileTree(File root, Path rootPath) {
        List<File> files = new ArrayList<>();

        String subfolder = rootPath.relativize(root.toPath()).toString();
        // Add slash if needed
        subfolder = subfolder.length() > 0 ? subfolder + File.separatorChar : "";

        File[] children = root.listFiles(ensFileFilter);
        if (children != null) {
            // Get each .ens file in the folder
            for (File file : children) {
                if (file.isDirectory()) {
                    files.addAll(getFileTree(file, rootPath));
                } else {
                    files.add(new File(subfolder + file.getName()));
                }
            }
        }

        // Make sure subfiles are last
        Collections.reverse(files);
        return files;
    }

    public static boolean compileSingle(File root, File source, File outputFolder) {
        String filePath = (root == null ? "" : root.getPath() + File.separatorChar) + source.getPath();
        try (FileReader reader = new FileReader(filePath)) {
            // If we're compiling a single file the file path shouldn't be added to the output path
            String relativePath = root == null ? source.getName() :  source.getPath();
            // Add output folder + subfolder + file name
            String output = outputFolder.getName() + File.separatorChar + relativePath.replace(".ens", ".cpp");
            // Print result
            System.out.println("Compiling " + source.getPath() + " to " + output);
            return compileSingle(reader, outputFolder);
        } catch (IOException e) {
            System.err.println("ERROR: Couldn't read " + source);
            e.printStackTrace();
            return false;
        }
    }

    public static boolean compileSingle(Reader source, File outputFolder) {
        try (BufferedReader reader = new BufferedReader(source)) {
            String code = reader.lines().collect(Collectors.joining("\n"));
            List<Token> tokens = Tokenizer.tokenize(code);
            return true;
        } catch (IOException e) {
            e.printStackTrace();
            return false;
        }
    }
}
