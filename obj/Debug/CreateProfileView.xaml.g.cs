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
    
    
    public partial class CreateProfileView : ContentPage {
        
        private ListView listHours;
        
        private ListView listMins;
        
        private ListView listSecs;
        
        private Label lblSelectedTime;
        
        private void InitializeComponent() {
            this.LoadFromXaml(typeof(CreateProfileView));
            listHours = this.FindByName<ListView>("listHours");
            listMins = this.FindByName<ListView>("listMins");
            listSecs = this.FindByName<ListView>("listSecs");
            lblSelectedTime = this.FindByName<Label>("lblSelectedTime");
        }
    }
}