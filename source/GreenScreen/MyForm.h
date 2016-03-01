#pragma once
#include "opencv/cv.hpp"
#include "EDSDK.h"
#include "EDSDKErrors.h"
#include "EDSDKTypes.h"
#include <Windows.h>

#define SRCCOPY2             (unsigned long)0x00CC0020

namespace GreenScreen {

	using namespace System;
	using namespace System::ComponentModel;
	using namespace System::Collections;
	using namespace System::Windows::Forms;
	using namespace System::Data;
	using namespace System::Drawing;
	using namespace System::IO;
	using namespace System::Collections::Generic;
	using namespace System::Runtime::InteropServices;
	using namespace cv;
	using namespace std;

	//OUTSIDE VARS
	//canon vars
	EdsError err = EDS_ERR_OK;
	bool isSDKLoaded = false;
	bool isOpen = false;
	bool allowPrint = true;
	bool isRequesting = false;
	EdsCameraRef camera = NULL;
	char* fileName;
	// live Stream canon vars
	EdsEvfImageRef evfImage = NULL;
	EdsStreamRef stream = NULL;
	unsigned char* data = NULL;
	unsigned long size = 0;
	bool isLiveStream = false;

	//******************************************
	//Image processing var
	int xCoordSample = 10;
	int yCoordSample = 10;
	int hueVar = 20;
	int saturationVar = 50;
	int valueVar = 65;
	Mat background;
	Mat backgroundLive;
	Mat foreground;
	Mat foregroundLive;
	Mat composeImage;
	Mat composeImageLive;

	//OUTSIDE METHODS
	//get the first CANON camera connected to pc
	EdsError getFirstCamera(EdsCameraRef *camera)
	{
		EdsError err = EDS_ERR_OK;
		EdsCameraListRef cameraList = NULL;
		EdsUInt32 count = 0;
		// Get camera list
		err = EdsGetCameraList(&cameraList);
		// Get number of cameras
		if (err == EDS_ERR_OK)
		{
			err = EdsGetChildCount(cameraList, &count);
			if (count == 0) err = EDS_ERR_DEVICE_NOT_FOUND;
		}
		// Get first camera retrieved
		if (err == EDS_ERR_OK)err = EdsGetChildAtIndex(cameraList, 0, camera);

		// Release camera list
		if (cameraList != NULL)
		{
			EdsRelease(cameraList);
			cameraList = NULL;
		}
		return err;
	}

	//Canon event handler
	static EdsError EDSCALLBACK handleObjectEvent(EdsObjectEvent event, EdsBaseRef object, EdsVoid * context)
	{
		EdsError err = EDS_ERR_OK;
		if (event == kEdsObjectEvent_DirItemRequestTransfer)
		{
			EdsStreamRef stream = NULL;
			EdsDirectoryItemInfo dirItemInfo;
			err = EdsGetDirectoryItemInfo(object, &dirItemInfo);
			err = EdsCreateFileStream(fileName, kEdsFileCreateDisposition_CreateAlways, kEdsAccess_ReadWrite, &stream);
			err = EdsDownload(object, dirItemInfo.size, stream);
			err = EdsDownloadComplete(object);
			EdsRelease(stream);
			stream = NULL;
			return err;
		}
		if (object) EdsRelease(object);
	}

	//get the CANON camera name by reference
	static char* getDeviceName(EdsCameraRef &camera)
	{
		EdsDeviceInfo deviceInfo;
		EdsError err = EDS_ERR_OK;
		if (err == EDS_ERR_OK)
		{
			err = EdsGetDeviceInfo(camera, &deviceInfo);
			if (err == EDS_ERR_OK && camera == NULL) err = EDS_ERR_DEVICE_NOT_FOUND;
		}
		if (err == EDS_ERR_OK) return deviceInfo.szDeviceDescription;
		return "";
	}

	float clamp(int n, int lower, int upper) {
		return max(lower, min(n, upper));
	}

	int lerp(int a, int b, float f)
	{
		return a + (int)(f * (float)(b - a));
	}

	//compute a chroma key: by default sample (10,10) image coords, and default = alive process for fast solutions
	void chromaKey(Mat& image, bool isLive = true)
	{
		if (image.empty()) return;

		//convert image to HSV and create a empty mask with same size
		Mat hsv;
		Mat mask = Mat(cv::Size(image.cols, image.rows), CV_8UC1);
		cvtColor(image, hsv, CV_BGR2HSV);

		//take the sample to chroma
		int xCoord, yCoord;
		xCoord = xCoordSample;
		yCoord = yCoordSample;

		if (!isLive)
		{
			float scaleFactor = (5184.00f / 720.00f);
			int xNew = xCoord * scaleFactor;
			int yNew = yCoord * scaleFactor;
			if (xNew > 0 && xNew > 5184 && yNew > 0 && yNew < 3400)
			{
				xCoord *= scaleFactor;
				yCoord *= scaleFactor;
			}
		}

		Vec3b sample = hsv.at<Vec3b>(xCoord, yCoord);
		uchar hue = sample[0];
		uchar saturation = sample[1];
		uchar value = sample[2];

		//define min and max threshold for this sample
		Vec3b rangeMin = Vec3b(clamp(hue - hueVar, 0, 255), clamp(saturation - saturationVar, 0, 255), clamp(value - valueVar, 0, 255));
		Vec3b rangeMax = Vec3b(clamp(hue + hueVar, 0, 255), 255, clamp(value + valueVar, 0, 255));

		//from range get mask in alpha
		inRange(hsv, rangeMin, rangeMax, mask);
		mask.convertTo(mask, CV_8UC1);

		//dilate 2 pixels and invert mask
		dilate(mask, mask, Mat(), cv::Point(-1, -1), 2, 1, 1);
		bitwise_not(mask, mask);

		//blur mask for better results
		blur(mask, mask, cv::Size(3, 3));
		//return at original 
		cvtColor(hsv, image, CV_HSV2BGR);
		//compute the chroma mask
		for (int y = 0; y < image.rows; y++)
		{
			for (int x = 0; x < image.cols; x++)
			{
				int val = mask.at<uchar>(cv::Point(x, y));
				if (isLive)
				{
					Vec3b BGpixel = backgroundLive.at<Vec3b>(cv::Point(x, y));
					if (val < 255)
					{
						image.at<Vec3b>(cv::Point(x, y)) = BGpixel;
					}
				}
				else
				{
					Vec3b BGpixel = background.at<Vec3b>(cv::Point(x, y));
					if (val < 255)
					{
						image.at<Vec3b>(cv::Point(x, y)) = BGpixel;
					}
				}
			}
		}
	}

	void overlayImage(const cv::Mat &background, const cv::Mat &foreground, cv::Mat &output, cv::Point2i location)
	{
		background.copyTo(output);
		// start at the row indicated by location, or at row 0 if location.y is negative.
		for (int y = max(location.y, 0); y < background.rows; ++y)
		{
			int fY = y - location.y; // because of the translation

			// we are done of we have processed all rows of the foreground image.
			if (fY >= foreground.rows) break;

			// start at the column indicated by location, 
			// or at column 0 if location.x is negative.
			for (int x = max(location.x, 0); x < background.cols; ++x)
			{
				int fX = x - location.x; // because of the translation.

				// we are done with this row if the column is outside of the foreground image.
				if (fX >= foreground.cols)break;

				// determine the opacity of the foregrond pixel, using its fourth (alpha) channel.
				double opacity = ((double)foreground.data[fY * foreground.step + fX * foreground.channels() + 3]) / 255.;

				// but only if opacity > 0.
				for (int c = 0; opacity > 0 && c < output.channels(); ++c)
				{
					unsigned char foregroundPx =
						foreground.data[fY * foreground.step + fX * foreground.channels() + c];
					unsigned char backgroundPx =
						background.data[y * background.step + x * background.channels() + c];
					output.data[y*output.step + output.channels()*x + c] =
						backgroundPx * (1. - opacity) + foregroundPx * opacity;
				}
			}
		}
	}

	static bool clearDirectory(System::String^ folder)
	{
		if (Directory::Exists(folder))
		{
			cli::array<System::String ^>^ files = Directory::GetFiles(folder);
			if (files->Length > 0)
			{
				for (int i = 0; i < files->Length; i++)
				{
					File::Delete(files[i]);
				}
				return true;
			}
		}
		return false;
	}

	//printing functions, this store the drivers and pointer to printer in order to call it
	struct printerInfo
	{
		LPCWSTR portName;
		LPCWSTR driver;
		LPCWSTR deviceName;
		LPCWSTR output = NULL;
	};

	// this return the printer info about the primary printer setup in windows
	printerInfo getPrimaryPrinter()
	{
		printerInfo out;
		unsigned long size = 0;
		LPWSTR defaultName = NULL;
		//get default printer size info
		GetDefaultPrinter(NULL, &size);
		if (size)
		{
			TCHAR* buffer = new TCHAR[size];
			GetDefaultPrinter(buffer, &size);
			defaultName = buffer;
		}
		// if we found some printer then try to get all info to struct printerInfo
		if (defaultName)
		{
			out.deviceName = defaultName;
			LPBYTE pPrinterEnum;
			unsigned long pcbNeeded, pcbReturned;
			PRINTER_INFO_2* printerInfo = NULL;

			//list all printer in this pc
			EnumPrinters(PRINTER_ENUM_LOCAL, NULL, 2, NULL, 0, &pcbNeeded, &pcbReturned);

			//if we found some printer connected, then get the drivers and name and some stuff
			pPrinterEnum = new BYTE[pcbNeeded];
			if (EnumPrinters(PRINTER_ENUM_LOCAL, NULL, 2, pPrinterEnum, pcbNeeded, &pcbNeeded, &pcbReturned))
			{
				printerInfo = ((PRINTER_INFO_2*)pPrinterEnum);
				wstring wsdefaultName(defaultName);
				string sdefaultName(wsdefaultName.begin(), wsdefaultName.end());

				//fill the printerInfo
				for (unsigned int i = 0; i < pcbReturned; i++)
				{
					LPWSTR name = printerInfo[i].pPrinterName;
					wstring wsname(name);
					string sname(wsname.begin(), wsname.end());
					if (sname == sdefaultName)
					{
						out.driver = printerInfo[i].pDriverName;
						out.portName = printerInfo[i].pPortName;
					}
				}
			}
		}
		return out;
	}

	// call the printer to print some file in some folder, we need pass .bmp file
	void printInPrinter( Mat out)
	{
		if (out.empty()) return;

		printerInfo dev = getPrimaryPrinter();

		int width = 2700;
		int height = 4050;
		Mat to;
		resize(out, to, cv::Size(width, height));

		//open printer
		HBITMAP hBMP = CreateBitmap(width, height, 1, 24, to.data);
		HDC printer = CreateDC(dev.driver, dev.deviceName, dev.portName, NULL);

		if (printer)
		{
			HDC hdc = CreateCompatibleDC(printer);
			SelectObject(hdc, hBMP);
			Escape(printer, STARTDOC, 8, "Happy-Doc", NULL);
			BitBlt(printer, 0, 0, width, height, hdc, 1, 1, SRCCOPY2);
			Escape(printer, NEWFRAME, 0, NULL, NULL);
			Escape(printer, ENDDOC, 0, NULL, NULL);

			DeleteDC(printer);
		}
	}


	/// <summary>
	/// Summary for MyForm
	/// </summary>
	public ref class MyForm : public System::Windows::Forms::Form
	{
	public:

		MyForm(void)
		{
			InitializeComponent();
			//
			//TODO: Add the constructor code here
			//
		}

		//VARS
		System::String^ tmpPath = Directory::GetCurrentDirectory() + "\\tmp";
		System::String^ savePath = Directory::GetCurrentDirectory() + "\\Save";
		System::String^ resourcePath = Directory::GetCurrentDirectory() + "\\Resource";
		System::String^ bgFolder = resourcePath + "/background";
		System::String^ fgFolder = resourcePath + "/foreground";
		int saveIncremental = 0;

		List<System::String^> backgroundList;
		List<System::String^> foregroundList;
		int resouceSize = -1;
		int liveStreamWidth = 0;
		int liveStreamHeight = 0;
		bool isWideScreen = true;
		int offsetScreenX = 100;
		int offsetScreenY = 150;


		//FUNCTIONS
		inline Mat getBackground(int index)
		{
			Mat out;
			if (index >= 0 && index <= resouceSize)
			{
				if (File::Exists(backgroundList[index]))
				{
					char* imgPath = (char*)(void*)Marshal::StringToHGlobalAnsi(backgroundList[index]);
					out = imread(imgPath);
				}
			}
			return out;
		}

		inline Mat getForeground(int index)
		{
			Mat out;
			if (index >= 0 || index <= resouceSize)
			{
				if (File::Exists(foregroundList[index]))
				{
					char* imgPath = (char*)(void*)Marshal::StringToHGlobalAnsi(foregroundList[index]);
					out = imread(imgPath, CV_LOAD_IMAGE_UNCHANGED);
				}
			}
			return out;
		}

		inline void setRandomImageSet()
		{
			Random^ r = gcnew Random();
			//background random
			int randBackgroundNum = r->Next(0, resouceSize + 1);
			background = getBackground(randBackgroundNum);
			if (!background.empty())
			{
				cv::resize(background, backgroundLive, cv::Size(liveStreamWidth, liveStreamHeight));
			}
			
			//foreground random
			int randForegroundNum = r->Next(0, resouceSize + 1);
			foreground = getForeground(randForegroundNum);
			if (!foreground.empty())
			{
				cv::resize(foreground, foregroundLive, cv::Size(liveStreamWidth, liveStreamHeight));
			}
		}

	protected:
		/// <summary>
		/// Clean up any resources being used.
		/// </summary>
		~MyForm()
		{
			if (components)
			{
				delete components;
			}
		}

	private: System::Windows::Forms::Button^  button1;
	private: System::Windows::Forms::Button^  button2;
	private: System::Windows::Forms::Button^  button3;
	private: System::Windows::Forms::PictureBox^  pictureBox1;
	private: System::Windows::Forms::CheckBox^ checkBox1;
	private: System::Windows::Forms::Timer^  timer1;
	private: System::Windows::Forms::HScrollBar^  hScrollBar1;
	private: System::Windows::Forms::HScrollBar^  hScrollBar2;
	private: System::Windows::Forms::HScrollBar^  hScrollBar3;

	private:
		/// <summary>
		/// Required designer variable.
		/// </summary>
		System::ComponentModel::Container ^components;

#pragma region Windows Form Designer generated code
		/// <summary>
		/// Required method for Designer support - do not modify
		/// the contents of this method with the code editor.
		/// </summary>
		void InitializeComponent(void)
		{
			//THIS STUFF BEGINS
			//create directories
			if (!Directory::Exists(resourcePath))
			{
				Directory::CreateDirectory(resourcePath);
				Console::WriteLine("Add resource check the resource folder content for errors!!!!!");
			}
			if (!Directory::Exists(bgFolder))
			{
				Directory::CreateDirectory(bgFolder);
				Console::WriteLine("Add background folder");
			}
			if (!Directory::Exists(fgFolder))
			{
				Directory::CreateDirectory(fgFolder);
				Console::WriteLine("Add foreground folder");
			}
			if (!Directory::Exists(savePath))
			{
				Directory::CreateDirectory(savePath);
				Console::WriteLine("Add save folder");
			}
			if (!Directory::Exists(tmpPath))
			{
				Directory::CreateDirectory(tmpPath);
				Console::WriteLine("Add temporal folder");
			}

			//clear temporal folder
			if (clearDirectory(tmpPath))
			{
				Console::WriteLine("cleaning the temporal folder...");
			}

			//load resources
			if (Directory::Exists(resourcePath))
			{
				if (Directory::Exists(bgFolder) && Directory::Exists(fgFolder))
				{
					cli::array<System::String^>^ bgFiles = Directory::GetFiles(bgFolder);
					cli::array<System::String^>^ fgFiles = Directory::GetFiles(fgFolder);
					if (bgFiles->Length == 0)
					{
						Console::WriteLine("not resources, exit app");
						return;
					}
					if (bgFiles->Length == fgFiles->Length)
					{
						for (int i = 0; i < bgFiles->Length; i++)
						{
							backgroundList.Add(bgFiles[i]);
							foregroundList.Add(fgFiles[i]);
							resouceSize++;
						}
						Console::WriteLine("loading resources success!");
					}
					else
					{
						Console::WriteLine("mismacth on background folder size and foreground folder size, checks the same file lengh in both");
						return;
					}
				}
			}

			//GetLastSave
			if (Directory::Exists(savePath))
			{
				cli::array<System::String^>^ files = Directory::GetFiles(savePath);
				if (files->Length > 0)
				{
					System::String^ last = files[files->Length - 1];
					cli::array<System::String^>^ cropLast = last->Split('_');
					saveIncremental = int::Parse(cropLast[cropLast->Length - 1]->Split('.')[0]);
				}
			}

			//setup resources size
			Mat mat1 = getBackground(1);
			if (!mat1.empty())
			{
				int width = mat1.cols;
				int height = mat1.rows;
				if (width > 1024 || height > 1024)
				{
					float aspect = width / (height *1.00f);
					if (aspect < 1)
					{
						isWideScreen = false;
						liveStreamHeight = 700;
						liveStreamWidth = (700.00f / height) * width;
					}
					else
					{
						isWideScreen = true;
						liveStreamWidth = 700;
						liveStreamHeight = (700.00f / width) * height;
					}

					Console::WriteLine("setting live stream screen size at: " + liveStreamWidth + " : " + liveStreamHeight);
				}
				else
				{
					liveStreamHeight = width;
					liveStreamWidth = width;
				}
			}
			this->components = gcnew System::ComponentModel::Container();
			System::ComponentModel::ComponentResourceManager^  resources = (gcnew System::ComponentModel::ComponentResourceManager(MyForm::typeid));
			this->pictureBox1 = (gcnew System::Windows::Forms::PictureBox());
			this->button1 = (gcnew System::Windows::Forms::Button());
			this->button2 = (gcnew System::Windows::Forms::Button());
			this->button3 = (gcnew System::Windows::Forms::Button());
			this->checkBox1 = (gcnew System::Windows::Forms::CheckBox());
			this->hScrollBar1 = (gcnew System::Windows::Forms::HScrollBar());
			this->hScrollBar2 = (gcnew System::Windows::Forms::HScrollBar());
			this->hScrollBar3 = (gcnew System::Windows::Forms::HScrollBar());
			this->timer1 = (gcnew System::Windows::Forms::Timer(this->components));
			(cli::safe_cast<System::ComponentModel::ISupportInitialize^>(this->pictureBox1))->BeginInit();
			this->SuspendLayout();
			// 
			// hScrollBar1
			// 
			this->hScrollBar1->AccessibleName = L"hue";
			this->hScrollBar1->Location = System::Drawing::Point(230, liveStreamHeight + 50);
			this->hScrollBar1->Maximum = 255;
			this->hScrollBar1->Minimum = 1;
			this->hScrollBar1->Name = L"hScrollBar1";
			this->hScrollBar1->Size = System::Drawing::Size(150, 10);
			this->hScrollBar1->TabIndex = 0;
			this->hScrollBar1->Value = hueVar;
			this->hScrollBar1->Scroll += gcnew System::Windows::Forms::ScrollEventHandler(this, &MyForm::hScrollBar1_Scroll);
			// 
			// hScrollBar2
			// 
			this->hScrollBar2->AccessibleName = L"saturation";
			this->hScrollBar2->Location = System::Drawing::Point(230, liveStreamHeight + 63);
			this->hScrollBar2->Maximum = 255;
			this->hScrollBar2->Minimum = 1;
			this->hScrollBar2->Name = L"hScrollBar2";
			this->hScrollBar2->Size = System::Drawing::Size(150, 10);
			this->hScrollBar2->TabIndex = 0;
			this->hScrollBar2->Value = saturationVar;
			this->hScrollBar2->Scroll += gcnew System::Windows::Forms::ScrollEventHandler(this, &MyForm::hScrollBar2_Scroll);
			// 
			// hScrollBar3
			// 
			this->hScrollBar3->AccessibleName = L"value";
			this->hScrollBar3->Location = System::Drawing::Point(230, liveStreamHeight + 76);
			this->hScrollBar3->Maximum = 255;
			this->hScrollBar3->Minimum = 1;
			this->hScrollBar3->Name = L"hScrollBar3";
			this->hScrollBar3->Size = System::Drawing::Size(150, 10);
			this->hScrollBar3->TabIndex = 0;
			this->hScrollBar3->Value = valueVar;
			this->hScrollBar3->Scroll += gcnew System::Windows::Forms::ScrollEventHandler(this, &MyForm::hScrollBar2_Scroll);
			// 
			// button1
			// 
			this->button1->ImageAlign = System::Drawing::ContentAlignment::BottomLeft;
			this->button1->Location = System::Drawing::Point(23, liveStreamHeight + 50);
			this->button1->Name = L"button1";
			this->button1->Size = System::Drawing::Size(126, 34);
			this->button1->TabIndex = 0;
			this->button1->Text = L"Take Photo";
			this->button1->UseVisualStyleBackColor = true;
			this->button1->Click += gcnew System::EventHandler(this, &MyForm::button1_Click);
			// 
			// button2
			//
			this->button2->ImageAlign = System::Drawing::ContentAlignment::BottomRight;
			this->button2->Location = System::Drawing::Point(liveStreamWidth - 70, liveStreamHeight + 50);
			this->button2->Name = L"button2";
			this->button2->Size = System::Drawing::Size(128, 34);
			this->button2->TabIndex = 1;
			this->button2->Text = L"Exit";
			this->button2->UseVisualStyleBackColor = true;
			this->button2->Click += gcnew System::EventHandler(this, &MyForm::button2_Click);
			// 
			// button3
			//
			this->button3->ImageAlign = System::Drawing::ContentAlignment::BottomLeft;
			this->button3->Location = System::Drawing::Point(128 + 23, liveStreamHeight + 50);
			this->button3->Name = L"button3";
			this->button3->Size = System::Drawing::Size(70, 20);
			this->button3->TabIndex = 1;
			this->button3->Text = L"next";
			this->button3->UseVisualStyleBackColor = true;
			this->button3->Click += gcnew System::EventHandler(this, &MyForm::button3_Click);
			// 
			// pictureBox1
			// 
			this->pictureBox1->Location = System::Drawing::Point(offsetScreenX / 2 - 10, 27);
			this->pictureBox1->Name = L"pictureBox1";
			this->pictureBox1->Size = System::Drawing::Size(liveStreamWidth, liveStreamHeight);
			this->pictureBox1->TabIndex = 2;
			this->pictureBox1->TabStop = false;
			this->pictureBox1->Click += gcnew System::EventHandler(this, &MyForm::pictureBox1_Click);
			//
			// checkBox1
			//
			this->checkBox1->AutoSize = true;
			this->checkBox1->Checked = true;
			this->checkBox1->CheckState = System::Windows::Forms::CheckState::Checked;
			this->checkBox1->Location = System::Drawing::Point(128 + 24, liveStreamHeight  + 50 + 20);
			this->checkBox1->Name = L"checkBox1";
			this->checkBox1->Size = System::Drawing::Size(75, 17);
			this->checkBox1->TabIndex = 3;
			this->checkBox1->Text = L"Allow Print";
			this->checkBox1->UseVisualStyleBackColor = true;
			this->checkBox1->CheckedChanged += gcnew System::EventHandler(this, &MyForm::checkBox1_CheckedChanged);
			// 
			// timer1
			// 
			this->timer1->Enabled = true;
			this->timer1->Interval = 34;
			this->timer1->Tick += gcnew System::EventHandler(this, &MyForm::timer1_Tick);


						
			this->Size = System::Drawing::Size(liveStreamWidth + offsetScreenX, liveStreamHeight + offsetScreenY);
			this->Controls->Add(this->button2);
			this->Controls->Add(this->button1);
			this->Controls->Add(this->button3);
			this->Controls->Add(this->pictureBox1);
			this->Controls->Add(this->checkBox1);
			this->Controls->Add(this->hScrollBar1);
			this->Controls->Add(this->hScrollBar2);
			this->Controls->Add(this->hScrollBar3);
			this->Text = L"Green Screen";
			this->Padding = System::Windows::Forms::Padding(0);
			this->AutoScaleMode = System::Windows::Forms::AutoScaleMode::Font;
			(cli::safe_cast<System::ComponentModel::ISupportInitialize^>(this->pictureBox1))->EndInit();
			this->ResumeLayout(false);

			//INIALIZE ALL STUFF CAMERAS
			//initialize Canon SDK
			err = EdsInitializeSDK();
			if (err == EDS_ERR_OK)
			{
				isSDKLoaded = true;
			}

			if (err == EDS_ERR_OK)
			{
				err = getFirstCamera(&camera);
			}

			// Set object event handler
			if (err == EDS_ERR_OK)
			{
				err = EdsSetObjectEventHandler(camera, kEdsObjectEvent_All, handleObjectEvent, NULL);
			}

			//Open camera
			if (err == EDS_ERR_OK)
			{
				err = EdsOpenSession(camera);
				if (err == EDS_ERR_OK)
				{
					const char* cdeviceName = getDeviceName(camera);
					System::String^ deviceName;
					deviceName = gcnew System::String(cdeviceName);
					Console::WriteLine("access CANON success:   " + deviceName);
					isOpen = true;
				}
			}
			
			// Set camera properties for save image
			EdsInt32 saveTarget = kEdsSaveTo_Host;
			err = EdsSetPropertyData(camera, kEdsPropID_SaveTo, 0, 4, &saveTarget);
			EdsCapacity newCapacity = { 0x7FFFFFFF, 0x1000, 1 };
			err = EdsSetCapacity(camera, newCapacity);

			//Sleep(2000);
			evfImage = NULL;
			stream = NULL;
			data = NULL;
			size = 0;
			isLiveStream = false;

			// Start Live view  
			// Get the output device for the live view image
			EdsUInt32 device;
			err = EdsGetPropertyData(camera, kEdsPropID_Evf_OutputDevice, 0, sizeof(device), &device);

			// PC live view starts by setting the PC as the output device for the live view image. 
			if (err == EDS_ERR_OK)
			{
				device |= kEdsEvfOutputDevice_PC;
				err = EdsSetPropertyData(camera, kEdsPropID_Evf_OutputDevice, 0, sizeof(device), &device);
			}

			// Create memory stream
			err = EdsCreateMemoryStream(0, &stream);
			if (err == EDS_ERR_OK)
			{
				// Create EvfImageRef.
				err = EdsCreateEvfImageRef(stream, &evfImage);
			}

			if (err == EDS_ERR_OK && isOpen) {
				setRandomImageSet();
				Sleep(2000);
				isLiveStream = true;
				Console::WriteLine("access to live view success");
			}
			else
			{
				Console::WriteLine("something is wrong!: please check all is connected...");
			}

		}
#pragma endregion
		//THE PROGRAM BUTTONS
	

		private: System::Void button2_Click(System::Object^  sender, System::EventArgs^  e)
		{
			Console::WriteLine("Close connections...");
			if (isOpen)
			{
				// End session and release SDK
				EdsCloseSession(camera);
				EdsTerminateSDK();
			}
			if (isLiveStream)
			{
				EdsRelease(stream);
				stream = NULL;
				EdsRelease(evfImage);
				evfImage = NULL;
			}
			Application::Exit();
		}

		private: System::Void button3_Click(System::Object^  sender, System::EventArgs^  e)
		{
			setRandomImageSet();
		}

		private: System::Void checkBox1_CheckedChanged(System::Object^  sender, System::EventArgs^  e) 
		{
			if (checkBox1->CheckState == System::Windows::Forms::CheckState::Checked)
			{
				allowPrint = true;
				Console::WriteLine("allow print is true");
			}
			else
			{
				allowPrint = false;
				Console::WriteLine("allow print is false");
			}
		}

		//get sample
		private: System::Void pictureBox1_Click(System::Object^ sender, System::EventArgs^ e) {
			System::Drawing::Point^	p = this->PointToClient(Control::MousePosition);
			if (p->X > 0 && p->X < liveStreamWidth && p->Y > 0 && p->Y < liveStreamHeight)
			{
				xCoordSample = p->X;
				yCoordSample = p->Y;
				Console::WriteLine("Get Sample at:  " + p);
			}

		}
		private: System::Void hScrollBar1_Scroll(System::Object^  sender, System::Windows::Forms::ScrollEventArgs^  e) {
			hueVar = hScrollBar1->Value;
			Console::WriteLine("new hue at: " + hScrollBar1->Value);
		}

		private: System::Void hScrollBar2_Scroll(System::Object^  sender, System::Windows::Forms::ScrollEventArgs^  e) {
			saturationVar = hScrollBar2->Value;
			Console::WriteLine("new saturation value at: " + hScrollBar1->Value);
		}

		private: System::Void hScrollBar3_Scroll(System::Object^  sender, System::Windows::Forms::ScrollEventArgs^  e) {
			valueVar = hScrollBar3->Value;
			Console::WriteLine("new value at: " + hScrollBar1->Value);
		}

		private: System::Void button1_Click(System::Object^  sender, System::EventArgs^  e)
		{
			if (isOpen && Directory::Exists(tmpPath) && !isRequesting)
			{
				System::String^ newFile = tmpPath + "/temporal.jpg";
				char* out = (char*)(void*)Marshal::StringToHGlobalAnsi(newFile);
				fileName = out;
				//take picture with canon
				Console::WriteLine("Request for picture...");
				EdsSendCommand(camera, kEdsCameraCommand_TakePicture, 0);
				//begin request for print in tick
				isRequesting = true;
			}
		}

				 // tick 
		private: System::Void timer1_Tick(System::Object^  sender, System::EventArgs^  e) {
				//PRINTING*************************
			if (isRequesting)
			{
				if (File::Exists(tmpPath + "/temporal.jpg"))
				{
					System::String^ loadtmp = tmpPath + "/temporal.jpg";
					char* tempFilePath = (char*)(void*)Marshal::StringToHGlobalAnsi(loadtmp);
					Mat pic = imread(tempFilePath);
					if (!pic.empty())
					{
						int printWidth = pic.cols;
						int printHeight = pic.rows;
						Mat roiPrint;
						if (isWideScreen)
						{
							printWidth = pic.cols;
							printHeight = pic.rows;
							resize(background, background, cv::Size(printWidth, printHeight));
							resize(foreground, foreground, cv::Size(printWidth, printHeight));
							roiPrint = pic;
						}
						else
						{
							//int newWidth = decoded.cols * (decoded.rows / (liveStreamHeight * 1.00f));
							printWidth = pic.rows;
							printHeight = pic.cols;
							resize(background, background, cv::Size(printWidth, printHeight));
							resize(foreground, foreground, cv::Size(printWidth, printHeight));
							int newWidth = printHeight * (printHeight / (printWidth * 1.00f));
							resize(pic, pic, cv::Size(newWidth, printHeight));
							roiPrint = pic(cv::Rect((newWidth - printWidth) *.5f, 0, printWidth, printHeight));
						}

						//compute chromaKey
						chromaKey(roiPrint, false);

						//add foreground
						Mat output;
						overlayImage(roiPrint, foreground, output, Point2i(0, 0));

						//rotate image if wide
						if (isWideScreen)
						{
							resize(output, output, cv::Size(printWidth, printWidth));
							Mat rotateImg;
							Mat dst;
							Point2f pt(output.cols / 2, output.rows / 2);
							Mat r = getRotationMatrix2D(pt, 90, 1.0);
							warpAffine(output, rotateImg, r, cv::Size(output.cols, output.cols));
							resize(rotateImg, rotateImg, cv::Size(printHeight, printWidth));
							output = rotateImg;
						}

						//save image
						saveIncremental++;
						System::String^ saveNewPic = savePath + "/green_" + saveIncremental + ".png";
						char* fileWrite = (char*)(void*)Marshal::StringToHGlobalAnsi(saveNewPic);
						imwrite(fileWrite, output);
						Console::WriteLine("new image save at: " + saveNewPic);

						if (allowPrint)
						{
							printInPrinter(output);
							Console::WriteLine("printing!");
						}

					}
					isRequesting = false;
				}
			}
				
			try
			{
				//LIVE STREAM**********************
				if (isOpen && isLiveStream && !isRequesting)
				{
					// Download live view image data.
					err = EdsDownloadEvfImage(camera, evfImage);
					if (err != EDS_ERR_OK)
					{
						return;
					}

					// Get Pointer of evfStream
					err = EdsGetPointer(stream, (EdsVoid**)& data);
					if (err != EDS_ERR_OK) {
						Console::WriteLine("Download Live View Image Error in Function EdsGetPointer: ");
						return;
					}

					// Get Length of evfStream
					err = EdsGetLength(stream, &size);
					if (err != EDS_ERR_OK) {
						Console::WriteLine("Download Live View Image Error in Function EdsGetLength: ");
						return;
					}

					// Get image data array size
					EdsSize imageSize;
					err = EdsGetPropertyData(evfImage, kEdsPropID_Evf_CoordinateSystem, 0, sizeof(imageSize), &imageSize);
					if (err == EDS_ERR_OK)
					{

						//pass raw data to opencv
						Mat buffer = Mat(1, size, CV_8UC1, data);
						Mat decoded = imdecode(buffer, CV_LOAD_IMAGE_COLOR);

						if (!buffer.empty())
						{
							Mat resultMat;
							int streamWidth = decoded.cols;
							int streamHeight = decoded.rows;

							Mat roi;
							if (isWideScreen)
							{
								resize(decoded, decoded, cv::Size(liveStreamWidth, liveStreamHeight));
								roi = decoded;
							}
							else
							{
								int newWidth = decoded.cols * (decoded.rows / (liveStreamHeight * 1.00f));
								resize(decoded, decoded, cv::Size(newWidth, liveStreamHeight));
								roi = decoded(cv::Rect((newWidth - liveStreamWidth) *.5f, 0, liveStreamWidth, liveStreamHeight));
							}

							// compute the chroma key for green color at default sample x,y (10,10)							
							chromaKey(roi, true);

							//add foreground;
							overlayImage(roi, foregroundLive, resultMat, Point2i(0, 0));

							System::Drawing::Imaging::PixelFormat fmt = System::Drawing::Imaging::PixelFormat::Format24bppRgb;
							Bitmap^ result = gcnew Bitmap(liveStreamWidth, liveStreamHeight, fmt);
							System::Drawing::Imaging::BitmapData^ data = result->LockBits(System::Drawing::Rectangle(0, 0, liveStreamWidth, liveStreamHeight), System::Drawing::Imaging::ImageLockMode::ReadOnly, fmt);
							for (int y = 0; y < liveStreamHeight; y++)
							{
								unsigned char* ptr = reinterpret_cast<unsigned char*>((data->Scan0 + y * data->Stride).ToPointer());
								for (int x = 0; x < liveStreamWidth; x++)
								{
									Vec3b px = resultMat.at<Vec3b>(cv::Point(x, y));
									ptr[0] = px[0];
									ptr[1] = px[1];
									ptr[2] = px[2];
									ptr += 3;
								}
							}
							result->UnlockBits(data);

							this->pictureBox1->Image = result;
							this->pictureBox1->Refresh();
						}
					}
				}
			}catch(...){}

		}



	};
}
