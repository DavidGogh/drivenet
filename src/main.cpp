/////////////////////////////////////////////////////////////////////////////////////////
// This code contains NVIDIA Confidential Information and is disclosed
// under the Mutual Non-Disclosure Agreement.
//
// Notice
// ALL NVIDIA DESIGN SPECIFICATIONS AND CODE ("MATERIALS") ARE PROVIDED "AS IS" NVIDIA MAKES
// NO REPRESENTATIONS, WARRANTIES, EXPRESSED, IMPLIED, STATUTORY, OR OTHERWISE WITH RESPECT TO
// THE MATERIALS, AND EXPRESSLY DISCLAIMS ANY IMPLIED WARRANTIES OF NONINFRINGEMENT,
// MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
//
// NVIDIA Corporation assumes no responsibility for the consequences of use of such
// information or for any infringement of patents or other rights of third parties that may
// result from its use. No license is granted by implication or otherwise under any patent
// or patent rights of NVIDIA Corporation. No third party distribution is allowed unless
// expressly authorized by NVIDIA.  Details are subject to change without notice.
// This code supersedes and replaces all information previously supplied.
// NVIDIA Corporation products are not authorized for use as critical
// components in life support devices or systems without express written approval of
// NVIDIA Corporation.
//
// Copyright (c) 2015-2017 NVIDIA Corporation. All rights reserved.
//
// NVIDIA Corporation and its licensors retain all intellectual property and proprietary
// rights in and to this software and related documentation and any modifications thereto.
// Any use, reproduction, disclosure or distribution of this software and related
// documentation without an express license agreement from NVIDIA Corporation is
// strictly prohibited.
//
/////////////////////////////////////////////////////////////////////////////////////////


// DRIVENET COMMON
#include <drivenet/common/DriveNet.hpp>
#include <drivenet/common/common.hpp>
#include "drivenet/common/cv_connection.hpp"

//------------------------------------------------------------------------------
// Method declarations
//------------------------------------------------------------------------------
int main(int argc, const char **argv);
void runPipeline(DriveNet& drivenet, float32_t framerate);

//------------------------------------------------------------------------------
// Method implementations
//------------------------------------------------------------------------------
int main(int argc, const char **argv)
{
    // Program arguments
    ProgramArguments arguments({
        ProgramArguments::Option_t("camera-type", "ar0231-rccb"),
        ProgramArguments::Option_t("csi-port", "ab"),
        ProgramArguments::Option_t("camera-index", "0"),
        ProgramArguments::Option_t("slave", "0"),
        ProgramArguments::Option_t("input-type", "camera"),
    });

    // init framework
    initSampleApp(argc, argv, &arguments, NULL, 1280, 800);

    // set window resize callback
    //gWindow->setOnResizeWindowCallback(resizeWindowCallback);

    // init driveworks
    initSdk(&gSdk, gWindow);

//#ifdef VIBRANTE
    //gInputType = gArguments.get("input-type");
//#else
    //gInputType = "video";
//#endif

    // create HAL and camera
    dwImageProperties rawImageProps;
    dwCameraProperties cameraProps;
    bool sensorsInitialized = initSensors(&gSal, &gCameraSensor, &rawImageProps, &cameraProps, gSdk);

    if (sensorsInitialized && initPipeline(rawImageProps, cameraProps, gSdk)) {

        initRenderer(gRCBProperties, &gRenderer, gSdk, gWindow);

        DriveNet drivenet(gSdk);

        drivenet.initDetector(gRCBProperties, g_cudaStream);
        drivenet.initTracker(gRCBProperties, g_cudaStream);

        runPipeline(drivenet, cameraProps.framerate);
    }

    // release DW modules
    release();

    // release framework
    releaseSampleApp();

    return 0;
}

//------------------------------------------------------------------------------
void runPipeline(DriveNet& driveNet, float32_t framerate)
{
    OpenCVConnector cv;
    typedef std::chrono::high_resolution_clock myclock_t;
    typedef std::chrono::time_point<myclock_t> timepoint_t;
    auto frameDuration         = std::chrono::milliseconds((int)(1000 / framerate));
    timepoint_t lastUpdateTime = myclock_t::now();

    uint32_t frame = 0;
    uint32_t stopFrame = atoi(gArguments.get("stopFrame").c_str());

    gRun = gRun && dwSensor_start(gCameraSensor) == DW_SUCCESS;

    while (gRun && !gWindow->shouldClose()) {
        std::this_thread::yield();

        // run with at most 30FPS when the input is a video
        if (gInputType.compare("video") == 0) {
            std::chrono::milliseconds timeSinceUpdate =
                std::chrono::duration_cast<std::chrono::milliseconds>(myclock_t::now() - lastUpdateTime);
            if (timeSinceUpdate < frameDuration)
                continue;

            lastUpdateTime = myclock_t::now();
        }

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        dwCameraFrameHandle_t frameHandle;
        dwStatus result = dwSensorCamera_readFrame(&frameHandle, 0, 90000, gCameraSensor);

        if (result == DW_END_OF_STREAM) {
            std::cout << "Camera reached end of stream" << std::endl;
            dwSensor_reset(gCameraSensor);
            driveNet.resetDetector();
            driveNet.resetTracker();
            continue;
        } else if (result != DW_SUCCESS) {
            std::cerr << "Cannot read frame: " << dwGetStatusName(result) << std::endl;
            continue;
        }

        // Process every second frame in order to avoid latency on iGPU.
        if ((frame % 2 == 0) || (gInputType.compare("video") == 0)) {

            // get RCB and RGBA images for processing and rendering from the camera frame
            dwImageCUDA* rcbCudaImage = nullptr;
            dwImageGL*   rgbaGLImage  = nullptr;

            if (!getNextFrameImages(&rcbCudaImage, &rgbaGLImage, frameHandle))
            {
                gRun = false;
                continue;
            }

            // render RGBA GL image
            dwRenderer_renderTexture(rgbaGLImage->tex, rgbaGLImage->target, gRenderer);

            // Detect, track and render objects
            driveNet.inferDetectorAsync(rcbCudaImage);
            driveNet.inferTrackerAsync(rcbCudaImage);
            driveNet.processResults();
            for (size_t classIdx = 0; classIdx < driveNet.getNumClasses(); classIdx++) {

                // render bounding box
                dwRenderer_setColor(gBoxColors[classIdx % gMaxBoxColors], gRenderer);
                drawBoxesWithLabels(driveNet.getResult(classIdx),
                                    static_cast<float32_t>(gRCBProperties.width),
                                    static_cast<float32_t>(gRCBProperties.height),
                                    gLineBuffer, gRenderer);
            }

            // draw ROI of the first image
            drawROI(driveNet.drivenetParams.ROIs[0], DW_RENDERER_COLOR_LIGHTBLUE, gLineBuffer, gRenderer);

            // draw ROI of the second image
            drawROI(driveNet.drivenetParams.ROIs[1], DW_RENDERER_COLOR_YELLOW, gLineBuffer, gRenderer);

            // return used images
            returnNextFrameImages(rcbCudaImage, rgbaGLImage);

            gWindow->swapBuffers();
        }
        // Read buffer to cvMat and publish
        NvMediaImageSurfaceMap surfaceMap;
        if(NvMediaImageSurfaceMap(rgbaGLImage->img, NVMEDIA_IMAGE_ACCESS_READ, &surfacceMap)==NVMEDIA_SATUS_OK){
            cv.WriteToOpenCV((unsigned char*)surfaceMap.surfaceMap[0].mapping, rgbaGLImage->prop.width, rgbaGLImage->prop.height);
            NvMediaImageUnlock(rgbaGLImage->img);
        }else{
            std::cout << "img read fail \n" ;
        }
        dwSensorCamera_returnFrame(&frameHandle);
        ++frame;
        if(stopFrame && frame == stopFrame)
            break;
    }

    dwSensor_stop(gCameraSensor);
}
