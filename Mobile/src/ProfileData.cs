﻿using System;
using System.Runtime.InteropServices;

namespace FermentationController
{
	[StructLayout (LayoutKind.Explicit, Pack = 1)]
	public struct ProfileData
	{
		//0 = Simple Threshold, 1=Simple PID, 2=Complex Threshold 3=Complex PID
		[FieldOffset (0)]
		public byte Method;

		//Probe0 Assignment {0=Sensor, 1=Process, 2=Target}
		[FieldOffset (1)]
		public byte Probe0Assignment;

		//Probe1 Assignment {0=Sensor, 1=Process, 2=Target}
		[FieldOffset (2)]
		public byte Probe1Assignment;

		[FieldOffset (3)]
		public ProfileStepData[] StepData;
	}
}

