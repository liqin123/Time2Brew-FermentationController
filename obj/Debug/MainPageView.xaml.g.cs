//------------------------------------------------------------------------------
// <auto-generated>
//     This code was generated by a tool.
//     Runtime Version:4.0.30319.34014
//
//     Changes to this file may cause incorrect behavior and will be lost if
//     the code is regenerated.
// </auto-generated>
//------------------------------------------------------------------------------

namespace FermentationController {
    using System;
    using Xamarin.Forms;
    using Xamarin.Forms.Xaml;
    
    
    public partial class MainPageView : ContentPage {
        
        private Entry entryEcho;
        
        private Button btnEcho;
        
        private Label lblEchoReturnData;
        
        private Button btnSetTimeNow;
        
        private Button btnStatus;
        
        private Label lblStatusReturnData;
        
        private Button btnCreateProfile;
        
        private void InitializeComponent() {
            this.LoadFromXaml(typeof(MainPageView));
            entryEcho = this.FindByName<Entry>("entryEcho");
            btnEcho = this.FindByName<Button>("btnEcho");
            lblEchoReturnData = this.FindByName<Label>("lblEchoReturnData");
            btnSetTimeNow = this.FindByName<Button>("btnSetTimeNow");
            btnStatus = this.FindByName<Button>("btnStatus");
            lblStatusReturnData = this.FindByName<Label>("lblStatusReturnData");
            btnCreateProfile = this.FindByName<Button>("btnCreateProfile");
        }
    }
}