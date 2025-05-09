/*
 * Copyright (c) 2023, Azul Systems, Inc. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

import jdk.crac.Core;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracProcess;
import jdk.test.lib.crac.CracTest;

import java.io.File;
import java.io.FileInputStream;
import java.io.RandomAccessFile;

/**
 * @test
 * @library /test/lib
 * @compile ../../../../lib/jdk/test/lib/crac/CracBuilder.java
 * @build OpenFileDetectionTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class OpenFileDetectionTest implements CracTest {
    @Override
    public void test() throws Exception {
        CracProcess cp = new CracBuilder().engine(CracEngine.SIMULATE).captureOutput(true)
                .javaOption("jdk.crac.collect-fd-stacktraces", "true")
                .startCheckpoint();
        cp.outputAnalyzer()
                .shouldHaveExitValue(1)
                .shouldMatch("CheckpointOpenFileException: filename1.txt") // RandomAccessFile should have the expected format
                .shouldMatch("filename2.txt") // others are allowed to specify the path in some format
                .shouldContain("This file descriptor was created by "); // <thread> at <time> here
    }

    @Override
    public void exec() throws Exception {
        new File("filename1.txt").createNewFile();
        new File("filename2.txt").createNewFile();

        try (var file1 = new RandomAccessFile("filename1.txt", "r");
             var file2 = new FileInputStream("filename2.txt")) {
            Core.checkpointRestore();
        }
    }
}
