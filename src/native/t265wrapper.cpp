#include "t265wrapper.hpp"
#include <vector>
#include <fstream>
#include <iterator>
#include <cmath>
#include <iostream>
#include <chrono>
#include <thread>
#include <cstring>

// We use jlongs like pointers, so they better be large enough
static_assert(sizeof(jlong) >= sizeof(void *));

// Constants
constexpr auto originNodeName = "origin";
constexpr auto exportRelocMapStopDelay = std::chrono::seconds(10);

// We cache all of these because we can
jclass holdingClass = nullptr; // This should always be T265Camera jclass
jfieldID fieldID = nullptr;    // Field id for the field "nativeCameraObjectPointer"
jclass exception = nullptr;    // This is "CameraJNIException"

// We do this so we don't have to fiddle with files
// Most of the below fields are supposed to be "ignored"
// See https://github.com/IntelRealSense/librealsense/blob/master/doc/t265.md#wheel-odometry-calibration-file-format
constexpr auto odometryConfig = R"(
{
    "velocimeters": [
        {
            "scale_and_alignment": [
                1.0,
                0.0000000000000000,
                0.0000000000000000,
                0.0000000000000000,
                1.0,
                0.0000000000000000,
                0.0000000000000000,
                0.0000000000000000,
                1.0
            ],
            "noise_variance": %f,
            "extrinsics": {
                "T": [
                    %f,
                    %f,
                    0.0
                ],
                "T_variance": [
                    9.999999974752427e-7, 
                    9.999999974752427e-7, 
                    9.999999974752427e-7
                ],
                "W": [
                    0.0,
                    %f,
                    0.0
                ],
                "W_variance": [
                    9.999999974752427e-5, 
                    9.999999974752427e-5, 
                    9.999999974752427e-5
                ]
            }
        }
    ]
}
)";

jlong Java_com_spartronics4915_lib_sensors_T265Camera_newCamera(JNIEnv *env, jobject thisObj, jstring mapPath)
{
    try
    {
        ensureCache(env, thisObj);

        deviceAndSensors *devAndSensors = nullptr;
        try
        {
            devAndSensors = getDeviceFromClass(env, thisObj);
        }
        catch (std::runtime_error)
        {
        }
        if (devAndSensors && devAndSensors->isRunning)
            throw std::runtime_error("Can't make a new camera if the calling class already has one (you need to call free first)");

        auto pipeline = new rs2::pipeline();

        // Set up a config to ensure we only get tracking capable devices
        auto config = rs2::config();
        config.disable_all_streams();
        config.enable_stream(RS2_STREAM_POSE, RS2_FORMAT_6DOF);

        // Get the currently used device
        auto profile = config.resolve(*pipeline);
        auto device = profile.get_device();
        if (!device.is<rs2::tm2>())
        {
            pipeline->stop();
            throw std::runtime_error("The device you have plugged in is not tracking-capable");
        }

        // Get the odometry/pose sensors
        // For the T265 both odom and pose will be from the *same* sensor
        rs2::wheel_odometer *odom = nullptr;
        rs2::pose_sensor *pose = nullptr;
        for (const auto sensor : device.query_sensors())
        {
            if (sensor.is<rs2::wheel_odometer>())
                pose = new rs2::pose_sensor(sensor);
            if (sensor.is<rs2::pose_sensor>())
                odom = new rs2::wheel_odometer(sensor);
        }
        if (!odom)
            throw new std::runtime_error("Selected device does not support wheel odometry inputs");
        if (!pose)
            throw new std::runtime_error("Selected device does not have a pose sensor");

        // Ensure that pipeline->start(...) chooses the devices we just got
        auto poseSerial = pose->get_info(RS2_CAMERA_INFO_SERIAL_NUMBER);
        auto odomSerial = pose->get_info(RS2_CAMERA_INFO_SERIAL_NUMBER);
        config.enable_device(poseSerial);
        config.enable_device(odomSerial);

        // Import the relocalization map, if the path is nonempty
        auto pathNativeStr = env->GetStringUTFChars(mapPath, 0);
        if (std::strlen(pathNativeStr) > 0)
            importRelocalizationMap(pathNativeStr, pose);
        env->ReleaseStringUTFChars(mapPath, pathNativeStr);

        // Imagine writing a C++ wrapper but ignoring most C++ features
        // Looking at you jni.h
        // (See below for explanatory comment)
        JavaVM *jvm;
        int error = env->GetJavaVM(&jvm);
        if (error)
            throw std::runtime_error("Couldn't get a JavaVM object from the current JNI environment");
        auto globalThis = env->NewGlobalRef(thisObj); // Must be cleaned up later

        /*
         * We define a callback that will run in another thread.
         * This is why we must take care to preserve a pointer to
         * a jvm object, which we attach to the currrent thread
         * so we can get a valid environment object and call
         * callback in Java. We also make a global reference to
         * the current object, which will be cleaned up when the
         * user calls free() from Java.
        */
        auto consumerCallback = [jvm, globalThis](const rs2::frame &frame) {
            JNIEnv *env = nullptr;
            try
            {
                // Attaching the thread is expensive... TODO: Cache env?
                int error = jvm->AttachCurrentThread((void **)&env, nullptr);
                if (error)
                    throw std::runtime_error("Couldn't attach callback thread to jvm");

                auto poseData = frame.as<rs2::pose_frame>().get_pose_data();
                auto q = poseData.rotation;
                // rotation is a quaternion so we must convert to an euler angle (yaw)
                auto yaw = atan2f(2.0 * (q.z * q.w + q.x * q.y), -1.0 + 2.0 * (q.w * q.w + q.x * q.x));

                auto callbackMethodID = env->GetMethodID(holdingClass, "consumePoseUpdate", "(FFFFI)V");
                if (!callbackMethodID)
                    throw std::runtime_error("consumePoseUpdate method doesn't exist");

                env->CallVoidMethod(globalThis, callbackMethodID, poseData.translation.x, poseData.translation.y, yaw, poseData.velocity.x, poseData.tracker_confidence);
            }
            catch (std::exception &e)
            {
                /*
                 * Unfortunately if we get an exception while attaching the thread
                 * we can't throw into Java code, so we'll just print to stderr
                */
                if (env)
                    env->ThrowNew(exception, e.what());
                else
                    std::cerr << "Exception in frame consumer callback could not be thrown in Java code: " << e.what() << std::endl;
            }
        };

        // Start streaming
        pipeline->start(config, consumerCallback);

        return reinterpret_cast<jlong>(new deviceAndSensors(pipeline, odom, pose, globalThis));
    }
    catch (std::exception &e)
    {
        ensureCache(env, thisObj);
        env->ThrowNew(exception, e.what());
        return 0;
    }
    return 0;
}

void Java_com_spartronics4915_lib_sensors_T265Camera_sendOdometryRaw(JNIEnv *env, jobject thisObj, jint sensorId, jint frameNumber, jfloat xVel, jfloat yVel)
{
    try
    {
        ensureCache(env, thisObj);

        auto devAndSensors = getDeviceFromClass(env, thisObj);
        // jints are 32 bit and are signed so we have to be careful
        // jint shouldn't be able to be greater than UINT32_MAX, but we'll be defensive
        if (sensorId > UINT8_MAX || frameNumber > UINT32_MAX || sensorId < 0 || frameNumber < 0)
            env->ThrowNew(exception, "sensorId or frameNumber are out of range");

        devAndSensors->wheelOdometrySensor->send_wheel_odometry(sensorId, frameNumber, rs2_vector{.x = xVel, .y = yVel, .z = 0.0});
    }
    catch (std::exception &e)
    {
        env->ThrowNew(exception, e.what());
    }
}

void Java_com_spartronics4915_lib_sensors_T265Camera_exportRelocalizationMap(JNIEnv *env, jobject thisObj, jstring savePath)
{
    try
    {
        ensureCache(env, thisObj);

        auto pathNativeStr = env->GetStringUTFChars(savePath, 0);

        // Open file in binary mode
        auto file = std::ofstream(pathNativeStr, std::ios::binary);
        if (!file || file.bad())
            throw std::runtime_error("Couldn't open file to write a relocalization map");

        // Get data from sensor and write
        auto devAndSensors = getDeviceFromClass(env, thisObj);
        devAndSensors->pipeline->stop();
        devAndSensors->isRunning = false;

        // I know, this is really gross...
        // Unfortunately there is apparently no way to figure out if we're ready to export the map
        // https://github.com/IntelRealSense/librealsense/issues/4024#issuecomment-494258285
        std::this_thread::sleep_for(exportRelocMapStopDelay);

        // set_static_node fails if the confidence is not High
        auto success = devAndSensors->poseSensor->set_static_node(originNodeName, rs2_vector{0, 0, 0}, rs2_quaternion{0, 0, 0, 1});
        if (!success)
            throw std::runtime_error("Couldn't set static node while exporting a relocalization map... Your confidence must be \"High\" for this to work.");

        auto data = devAndSensors->poseSensor->export_localization_map();
        file.write(reinterpret_cast<const char *>(data.begin().base()), data.size());

        env->ReleaseStringUTFChars(savePath, pathNativeStr);

        // File automatically get closed at end of scope

        // TODO: Camera never gets started again...
        // If we try to call pipeline->start() it doesn't work. Bug in librealsense?
    }
    catch (std::exception &e)
    {
        // TODO: Make sleep time configurable (this will probably be a common issue users run into)
        auto what = std::string(e.what());
        what += " (If you got something like \"null pointer passed for argument \"buffer\"\", this means that you have a very large relocalization map, and you should increase exportRelocMapStopDelay in ";
        what += __FILE__;
        what += ")";
        env->ThrowNew(exception, what.c_str());
    }
}

void importRelocalizationMap(const char *path, rs2::pose_sensor *poseSensor)
{
    // Open file and make a vector to hold contents
    auto file = std::ifstream(path, std::ios::binary);
    if (!file || file.bad())
        throw std::runtime_error("Couldn't open file to read a relocalization map");

    auto dataVec = std::vector<uint8_t>(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());

    // Pass contents to the pose sensor
    auto success = poseSensor->import_localization_map(dataVec);
    if (!success)
        throw std::runtime_error("import_localization_map returned a value indicating failure");

    // TODO: Transform by get_static_node("origin", ...)

    file.close();
}

void Java_com_spartronics4915_lib_sensors_T265Camera_setOdometryInfo(JNIEnv *env, jobject thisObj, jfloat xOffset, jfloat yOffset, jfloat angOffset, jfloat measureCovariance)
{
    try
    {
        ensureCache(env, thisObj);

        auto size = snprintf(nullptr, 0, odometryConfig, measureCovariance, xOffset, yOffset, angOffset);
        char buf[size];
        snprintf(buf, size, odometryConfig, xOffset, yOffset, angOffset);
        auto vecBuf = std::vector<uint8_t>(*buf, *buf + size);

        auto devAndSensors = getDeviceFromClass(env, thisObj);
        devAndSensors->wheelOdometrySensor->load_wheel_odometery_config(vecBuf);
    }
    catch (std::exception &e)
    {
        env->ThrowNew(exception, e.what());
    }
}

void Java_com_spartronics4915_lib_sensors_T265Camera_free(JNIEnv *env, jobject thisObj)
{
    try
    {
        ensureCache(env, thisObj);

        auto devAndSensors = getDeviceFromClass(env, thisObj);
        if (devAndSensors->isRunning)
            devAndSensors->pipeline->stop();
        env->DeleteGlobalRef(devAndSensors->globalThis);

        delete devAndSensors;

        env->SetLongField(thisObj, fieldID, 0);
    }
    catch (std::exception &e)
    {
        env->ThrowNew(exception, e.what());
    }
}

deviceAndSensors *getDeviceFromClass(JNIEnv *env, jobject thisObj)
{
    auto pointer = env->GetLongField(thisObj, fieldID);
    if (pointer == 0)
        throw std::runtime_error("nativeCameraObjectPointer cannot be 0");
    return reinterpret_cast<deviceAndSensors *>(pointer);
}

void ensureCache(JNIEnv *env, jobject thisObj)
{
    if (!holdingClass)
    {
        auto lHoldingClass = env->GetObjectClass(thisObj);
        holdingClass = reinterpret_cast<jclass>(env->NewGlobalRef(lHoldingClass));
    }
    if (!fieldID)
    {
        fieldID = env->GetFieldID(holdingClass, "mNativeCameraObjectPointer", "J");
    }
    if (!exception)
    {
        auto lException = env->FindClass("com/spartronics4915/lib/sensors/T265Camera$CameraJNIException");
        exception = reinterpret_cast<jclass>(env->NewGlobalRef(lException));
    }
}