# -*- coding: UTF-8 -*-
import gettext

from Tools.Directories import SCOPE_LANGUAGE, resolveFilename

class Language:
	def __init__(self):
		gettext.install('enigma2', resolveFilename(SCOPE_LANGUAGE, ""), unicode=0, codeset="utf-8")
		self.activeLanguage = 0
		self.lang = {}
		self.langlist = []
		# FIXME make list dynamically
		# name, iso-639 language, iso-3166 country. Please don't mix language&country!
		# also, see "precalcLanguageList" below on how to re-create the language cache after you added a language
		self.addLanguage(_("English"), "en", "EN")
		self.addLanguage(_("German"), "de", "DE")
		self.addLanguage(_("Norwegian"), "no", "NO")
		self.addLanguage(_("Polish"), "pl", "PL")
		self.addLanguage(_("Russian"), "ru", "RU")

		self.callbacks = []

	def addLanguage(self, name, lang, country):
		try:
			self.lang[str(lang + "_" + country)] = ((name, lang, country))
			self.langlist.append(str(lang + "_" + country))
		except:
			print "Language " + str(name) + " not found"

	def activateLanguage(self, index):
		try:
			lang = self.lang[index]
			print "Activating language " + lang[0]
			gettext.translation('enigma2', resolveFilename(SCOPE_LANGUAGE, ""), languages=[lang[1]]).install(names=("ngettext"))
			self.activeLanguage = index
			for x in self.callbacks:
				x()
		except:
			print "Selected language does not exist!"

	def activateLanguageIndex(self, index):
		if index < len(self.langlist):
			self.activateLanguage(self.langlist[index])

	def getLanguageList(self):
		return [ (x, self.lang[x]) for x in self.langlist ]

	def getActiveLanguage(self):
		return self.activeLanguage
	
	def getActiveLanguageIndex(self):
		idx = 0
		for x in self.langlist:
			if x == self.activeLanguage:
				return idx
			idx += 1
		return None			

	def getLanguage(self):
		try:
			return str(self.lang[self.activeLanguage][1]) + "_" + str(self.lang[self.activeLanguage][2])
		except:
			return ''

	def addCallback(self, callback):
		self.callbacks.append(callback)

	def precalcLanguageList(self):
		# excuse me for those T1, T2 hacks please. The goal was to keep the language_cache.py as small as possible, *and* 
		# don't duplicate these strings.
		T1 = _("Please use the UP and DOWN keys to select your language. Afterwards press the OK button.")
		T2 = _("Language selection")
		l = open("language_cache.py", "w")
		print >>l, "# -*- coding: UTF-8 -*-"
		print >>l, "LANG_TEXT = {"
		for language in self.langlist:
			self.activateLanguage(language)
			print >>l, '"%s": {' % language
			print >>l, '\t"T1": "%s",' % (_(T1))
			print >>l, '\t"T2": "%s",' % (_(T2))
			print >>l, '},'
		print >>l, "}"

language = Language()
