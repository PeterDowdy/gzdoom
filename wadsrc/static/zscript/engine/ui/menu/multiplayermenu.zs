/*
** multiplayermenu.zs
** Browsing for games on the local network (the host and join forms are plain MENUDEF).
*/

// Lobbies found by broadcasting on the local network; see I_NetScanLAN.
struct LanGames native
{
	native static void Scan(int port, int waitMs);
	native static int Count();
	native static String Address(int index);
	native static String Description(int index);
	native static bool Joinable(int index);
}

// Searches again, in place.
class OptionMenuItemLanRescan : OptionMenuItemSubmenu
{
	OptionMenuItemLanRescan Init(String label)
	{
		Super.Init(label, 'None', 0, false);
		return self;
	}

	override bool Activate()
	{
		let m = MultiplayerLanMenu(Menu.GetCurrentMenu());
		if (m == null) return false;
		Menu.MenuSound("menu/choose");
		m.Fill();
		return true;
	}
}

// Joins the game at an address, the way "mpjoin" does.
class MultiplayerLanMenu : OptionMenu
{
	override void Init(Menu parent, OptionMenuDescriptor desc)
	{
		mDesc = desc;
		Fill();
		Super.Init(parent, desc);
	}

	// Rebuilds the whole list: the settings, the "search again" item and one item for each game found.
	void Fill()
	{
		int selected = mDesc.mSelectedItem;
		mDesc.mItems.Clear();
		mDesc.mItems.Push(new("OptionMenuItemTextField").Init("Port", 'mp_port'));
		mDesc.mItems.Push(new("OptionMenuItemTextField").Init("Password, if it has one", 'mp_password'));
		mDesc.mItems.Push(new("OptionMenuItemLanRescan").Init("Search again"));
		mDesc.mItems.Push(new("OptionMenuItemStaticText").Init(" ", 0));

		LanGames.Scan(CVar.GetCVar('mp_port').GetInt(), 800);
		int count = LanGames.Count();
		if (count == 0)
		{
			mDesc.mItems.Push(new("OptionMenuItemStaticText").Init("No games found on this network", 0));
			mDesc.mItems.Push(new("OptionMenuItemStaticText").Init("A host has to be waiting in its lobby", 0));
		}
		for (int i = 0; i < count; i++)
		{
			if (LanGames.Joinable(i))
				mDesc.mItems.Push(new("OptionMenuItemCommand").Init(LanGames.Description(i), Name(String.Format("mpjoin %s", LanGames.Address(i)))));
			else
				mDesc.mItems.Push(new("OptionMenuItemStaticText").Init(LanGames.Description(i), 0));
		}

		mDesc.mSelectedItem = (selected >= 0 && selected < mDesc.mItems.Size() && mDesc.mItems[selected].Selectable()) ? selected : 2;
		mDesc.CalcIndent();
	}
}
