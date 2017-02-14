/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.android.settings.dualscreen;

import android.app.Activity;
import android.app.Dialog;
import android.app.DialogFragment;
import android.app.Fragment;
import android.content.ContentResolver;
import android.content.Context;
import android.content.DialogInterface;
import android.content.pm.PackageManager;
import android.database.DataSetObserver;
import android.graphics.drawable.Drawable;
import android.os.Bundle;
import android.preference.Preference;
import android.preference.PreferenceActivity;
import android.preference.PreferenceFragment;
import android.preference.PreferenceGroupAdapter;
import android.text.TextUtils;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.Menu;
import android.view.MenuInflater;
import android.view.MenuItem;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.ListAdapter;
import android.widget.ListView;
import android.widget.Switch;
import android.widget.TextView;
import android.database.ContentObserver;
import android.os.Handler;
import android.provider.Settings;
import android.app.ActivityManagerNative;
import android.app.IActivityManager;
import android.content.res.Configuration;
import android.os.RemoteException;
import com.android.settings.R;
import android.view.Gravity;

import com.android.settings.SettingsActivity;
import com.android.settings.SettingsPreferenceFragment;
import com.android.settings.widget.SwitchBar;


public class DualscreenSettings extends SettingsPreferenceFragment implements SwitchBar.OnSwitchChangeListener {
    private static final String TAG = DualscreenSettings.class.getSimpleName();
    static final boolean DEBUG = true;

    private static final String KEY_DUALSCREEN_MANUAL = "dualscreen_manual";
    private boolean mOpened;
    
    private final String store_key = Settings.System.DUAL_SCREEN_MODE;
    private final String icon_key = Settings.System.DUAL_SCREEN_ICON_USED;
    
    private final int DUAL_SCREEN_CLOSE = 0;
    private final int DUAL_SCREEN_OPEN = 1;
    
    private Context mContext;
    private SwitchBar mSwitchBar;

    @Override
    public void onAttach(Activity activity) {
        logd("onAttach(%s)", activity.getClass().getSimpleName());
        super.onAttach(activity);
        mContext = activity;
    }

    @Override
    public void onCreate(Bundle icicle) {
        logd("onCreate(%s)", icicle);
        super.onCreate(icicle);

        //mManual = (PreferenceScreen)findPreference(KEY_DUALSCREEN_MANUAL);        
    }

    @Override
    public void onSwitchChanged(Switch switchView, boolean isChecked) {
    	logd("-----------------------------isChecked:"+isChecked);
    	int value = DUAL_SCREEN_CLOSE;
    	
    	mContext.getContentResolver().unregisterContentObserver(mValueObserver);
    	mContext.getContentResolver().registerContentObserver(Settings.System.getUriFor(store_key), false, mValueObserver);
        if (isChecked) {
        	value = DUAL_SCREEN_OPEN;
        } else {
        	Settings.System.putInt(mContext.getContentResolver(), icon_key, 0);
        }
        Settings.System.putInt(mContext.getContentResolver(), store_key, value);
        handleStateChanged(value);
    }

    @Override
    public void onStart() {
        logd("onStart()");
        super.onStart();
    }

    @Override
    public void onDestroyView() {
        logd("onDestroyView()");
        super.onDestroyView();

        mSwitchBar.removeOnSwitchChangeListener(this);
        mSwitchBar.hide();
    }

    @Override
    public void onActivityCreated(Bundle savedInstanceState) {
        logd("onActivityCreated(%s)", savedInstanceState);
        super.onActivityCreated(savedInstanceState);

        ListView listView = getListView();
        listView.setItemsCanFocus(true);

        TextView emptyView = (TextView) getView().findViewById(android.R.id.empty);
        emptyView.setText(R.string.dualscreen_manual_summary);
	 emptyView.setTextSize(24);
	 emptyView.setGravity(Gravity.LEFT);
        listView.setEmptyView(emptyView);

        final SettingsActivity sa = (SettingsActivity) getActivity();
        mSwitchBar = sa.getSwitchBar();
        mSwitchBar.addOnSwitchChangeListener(this);
        mSwitchBar.show();
    }
    
    @Override
    public void onPause() {
        logd("onPause()");
        super.onPause();

        mContext.getContentResolver().unregisterContentObserver(mValueObserver);
    }

    @Override
    public void onResume() {
        logd("onResume()");
        super.onResume();
        handleStateChanged(getDualScreenValue());
        mContext.getContentResolver().registerContentObserver(Settings.System.getUriFor(store_key), false, mValueObserver);
    }
    
    private int getDualScreenValue(){
    	return Settings.System.getInt(mContext.getContentResolver(), store_key, DUAL_SCREEN_CLOSE);
    }
    
    void handleStateChanged(int state) {
        switch (state) {
            case DUAL_SCREEN_CLOSE:
            	mSwitchBar.setChecked(false);
                break;
            case DUAL_SCREEN_OPEN:
            	mSwitchBar.setChecked(true);
                break;
        }
    }

    private final ContentObserver mValueObserver = new ContentObserver(new Handler()) {
        @Override
        public void onChange(boolean selfChange) {
            final boolean enable = (DUAL_SCREEN_CLOSE != getDualScreenValue());
		Log.d(TAG, "onchagne enable=" + enable + ", selfChange=" + selfChange);
			
		try {
	            IActivityManager am = ActivityManagerNative.getDefault();
	            Configuration config = am.getConfiguration();

	            // Will set userSetLocale to indicate this isn't some passing default - the user
	            // wants this remembered
	            config.setDualScreenFlag(enable);

	            am.updateConfiguration(config);
		
	        } catch (RemoteException e) {
	            // Intentionally left blank
	        }
        }
    };

    private static void logd(String msg, Object... args) {
        if (DEBUG)
            Log.d(TAG, args == null || args.length == 0 ? msg : String.format(msg, args));
    }
    
}
