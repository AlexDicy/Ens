package ens.lang;

import java.io.*;

public class Main {

    public static void main(String[] args) throws IOException {
        Reader reader = new BufferedReader(new FileReader("test.ens"));
        StreamTokenizer tokenizer = new StreamTokenizer(reader);
        //tokenizer.slashSlashComments(true);
        //tokenizer.slashStarComments(true);

        int token;
        while ((token = tokenizer.nextToken()) != StreamTokenizer.TT_EOF) {
            System.out.println(tokenizer);
        }
    }
}
