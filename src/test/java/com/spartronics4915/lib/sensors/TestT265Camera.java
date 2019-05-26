package com.spartronics4915.lib.sensors;

import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.fail;

import java.nio.file.Path;
import java.nio.file.Paths;

import com.spartronics4915.lib.math.geometry.Pose2d;
import com.spartronics4915.lib.math.geometry.Twist2d;
import com.spartronics4915.lib.util.Logger;

import org.junit.jupiter.api.Tag;
import org.junit.jupiter.api.Test;

public class TestT265Camera
{

    private volatile boolean mDataRecieved = false;

    @Tag("hardwareDependant")
    @Test
    public void testNewCamera() throws InterruptedException
    {
        // This one is a little hard to unit test because we haven't simulated the hardware
        // We mostly just make sure that we can get through this sequence without throwing an exception
        // The rest is just a few sanity checks

        T265Camera cam = null;
        try
        {
            cam = new T265Camera((Pose2d p, T265Camera.PoseConfidence c) ->
            {
                mDataRecieved = true;
            }, new Pose2d(), 0f);

            // Just make sure this doesn't throw
            cam.sendOdometry(0, 0, new Twist2d(0, 0, 0));

            cam.start();
            Logger.debug("Waiting 1 seconds to recieve data...");
            Thread.sleep(1000);
            assertTrue(mDataRecieved, "No pose data was recieved after 1 second... Try moving the camera?");
            cam.stop();

            Logger.debug("Got pose data, exporting relocalization map to java.io.tmpdir...");
            Path mapPath = Paths.get(System.getProperty("java.io.tmpdir"), "map.bin").toAbsolutePath();
            cam.exportRelocalizationMap(mapPath.toString());

            if (mapPath.toFile().length() <= 0)
                fail("Relocalization map file length was 0");
            Logger.debug("Successfuly saved relocalization map, we will now try to import it");

            // The only check for this one is that it doesn't throw
            cam.importRelocalizationMap(mapPath.toString());
            
            Logger.debug("Map imported without errors");
        }
        finally
        {
            if (cam != null)
                cam.free();
        }
    }

}