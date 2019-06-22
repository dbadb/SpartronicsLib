package com.spartronics4915.lib.subsystems;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

public class TestSubsystem extends Subsystem
{
    @Test
    public void testInitChecks()
    {
        assertFalse(super.isInitialized());
        super.logInitialized(true);
        assertTrue(super.isInitialized());
        super.logInitialized(false);
        assertFalse(super.isInitialized());
    }

    @Test
    public void testGetName()
    {
        assertTrue(super.getName().equals("TestSubsystem"));
    }

    @Override
    public void outputTelemetry()
    {
    }

    @Override
    public void stop()
    {
    }
}