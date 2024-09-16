#include <windows.h>
#include <atlbase.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <cstdlib>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

#define HRCHECK(__expr) {auto __hr=(__expr);if(FAILED(__hr)){wprintf(L"FAILURE 0x%08X (%i)\n\tline: %u file: '%s'\n\texpr: '" _CRT_WIDE(#__expr) L"'\n",__hr,__hr,__LINE__,_CRT_WIDE(__FILE__));_CrtDbgBreak();}}
#define WIN32CHECK(__expr) {if(!(__expr)){auto __hr=HRESULT_FROM_WIN32(GetLastError());{wprintf(L"FAILURE 0x%08X (%i)\n\tline: %u file: '%s'\n\texpr: '" _CRT_WIDE(#__expr) L"'\n",__hr,__hr,__LINE__,_CRT_WIDE(__FILE__));_CrtDbgBreak();}}}

static HRESULT SetOutputType(IMFTransform* transform, GUID format)
{
	DWORD index = 0;
	do
	{
		CComPtr<IMFMediaType> outputType;
		auto hr = transform->GetOutputAvailableType(0, index++, &outputType);
		if (FAILED(hr))
			return hr;

		GUID guid;
		if (SUCCEEDED(outputType->GetGUID(MF_MT_SUBTYPE, &guid)) && guid == format)
		{
			HRCHECK(transform->SetOutputType(0, outputType, 0));
			return S_OK;
		}
	} while (true);
}

int main()
{
	HRCHECK(CoInitialize(nullptr));
	{
		HRCHECK(MFStartup(MF_VERSION));
		// open file
		auto file = CreateFile(L"h264.h264", GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		WIN32CHECK(file != INVALID_HANDLE_VALUE);

		// create H264 transform https://learn.microsoft.com/en-us/windows/win32/medfound/h-264-video-decoder
		CComPtr<IMFTransform> decoder;
		HRCHECK(decoder.CoCreateInstance(CLSID_MSH264DecoderMFT));

		// You can check in decoder attributes that MF_MT_FIXED_SIZE_SAMPLES is set to TRUE.
		// Calling GetOutputStreamInfo this will tell you the MFT cannot provide samples
		// as MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES and MFT_OUTPUT_STREAM_PROVIDES_SAMPLES are not set

		// So we don't know enough information yet, we'll feed input samples until we get MF_E_TRANSFORM_STREAM_CHANGE and then we'll provide a sample as per doc:
		//   "If the input type contains only these two attributes, the decoder will offer a default output type, which acts as a placeholder."
		//   "When the decoder receives enough input samples to produce an output frame, it signals a format change by returning MF_E_TRANSFORM_STREAM_CHANGE"
		UINT32 sampleSize = 0;

		// input type is H264
		CComPtr<IMFMediaType> inputType;
		HRCHECK(MFCreateMediaType(&inputType));
		HRCHECK(inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
		HRCHECK(inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
		HRCHECK(decoder->SetInputType(0, inputType, 0)); // video is id 0

		// get (and set) NV12 output type (could be I420, IYUV, YUY2, YV12)
		HRCHECK(SetOutputType(decoder, MFVideoFormat_NV12));

		do
		{
			// get a random chunk size between 500 and 1500
			DWORD chunkSize = 500 + (1000 * (RAND_MAX - std::rand())) / RAND_MAX;

			// create an MF input buffer & read into it
			CComPtr<IMFMediaBuffer> inputBuffer;
			HRCHECK(MFCreateMemoryBuffer(chunkSize, &inputBuffer));
			BYTE* chunk;
			HRCHECK(inputBuffer->Lock(&chunk, nullptr, nullptr));
			DWORD read;
			WIN32CHECK(ReadFile(file, chunk, chunkSize, &read, nullptr));
			HRCHECK(inputBuffer->SetCurrentLength(read));
			HRCHECK(inputBuffer->Unlock());
			if (read)
			{
				CComPtr<IMFSample> inputSample;
				HRCHECK(MFCreateSample(&inputSample));
				HRCHECK(inputSample->AddBuffer(inputBuffer));

				auto hr = decoder->ProcessInput(0, inputSample, 0);
				if (hr != MF_E_NOTACCEPTING) // just go on
				{
					HRCHECK(hr);
				}
			}
			else
			{
				// end of file, ask decoder to process all data from previous calls
				HRCHECK(decoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0));
			}

			CComPtr<IMFSample> outputSample;
			HRCHECK(MFCreateSample(&outputSample));
			MFT_OUTPUT_DATA_BUFFER outputBuffer{};
			outputBuffer.pSample = outputSample;

			if (sampleSize)
			{
				// now we know the size so we can (and must) allocate the MF output buffer
				CComPtr<IMFMediaBuffer> outputBuffer;
				HRCHECK(MFCreateMemoryBuffer(sampleSize, &outputBuffer));
				HRCHECK(outputSample->AddBuffer(outputBuffer));
			} // else just continue to process

			DWORD status = 0;
			auto hr = decoder->ProcessOutput(0, 1, &outputBuffer, &status);
			if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) // just go on
			{
				if (!read) // file is all read
					break;

				continue;
			}

			// https://learn.microsoft.com/en-us/windows/win32/medfound/handling-stream-changes
			if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
			{
				// get (and set) NV12 output type (could be I420, IYUV, YUY2, YV12)
				HRCHECK(SetOutputType(decoder, MFVideoFormat_NV12));

				// now get the sample size
				CComPtr<IMFMediaType> type;
				HRCHECK(decoder->GetOutputCurrentType(0, &type));
				HRCHECK(type->GetUINT32(MF_MT_SAMPLE_SIZE, &sampleSize));
				continue;
			}
			HRCHECK(hr);

			LONGLONG time, duration;
			HRCHECK(outputSample->GetSampleTime(&time));
			HRCHECK(outputSample->GetSampleDuration(&duration));
			wprintf(L"Sample time: %I64u ms duration: %I64u ms\n", time / 10000, duration / 10000);
		} while (true);

		// close file
		CloseHandle(file);
		HRCHECK(MFShutdown());
	}
	CoUninitialize();
	return 0;
}
